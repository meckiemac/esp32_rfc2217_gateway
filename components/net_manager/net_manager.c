#include "net_manager.h"

#include "wifi_config.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <lwip/apps/sntp.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <string.h>

static const char *TAG = "net_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAXIMUM_RETRY      5

static EventGroupHandle_t wifi_event_group;
static int s_retry_num;
static bool s_wifi_initialised;
static esp_event_handler_instance_t handler_any_id;
static esp_event_handler_instance_t handler_got_ip;

static void
initialise_sntp(void)
{
    ESP_LOGI(TAG, "Initialising SNTP");
    sntp_stop();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

static void
wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *) event_data;
        ESP_LOGW(TAG, "Disconnected from AP (reason %d)", disc ? disc->reason : -1);
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Retrying Wi-Fi connection (%d/%d)", s_retry_num, MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Obtained IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

bool
net_manager_init(void)
{
    if (s_wifi_initialised) {
        ESP_LOGW(TAG, "Network manager already initialised");
        return true;
    }

    wifi_event_group = xEventGroupCreate();
    if (!wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return false;
    }

    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (!netif) {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi STA");
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
        return false;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &handler_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &handler_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    s_retry_num = 0;
    s_wifi_initialised = true;
    ESP_LOGI(TAG, "Network manager initialised");
    return true;
}

static bool
wifi_connect_with_credentials(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = { 0 };
    strncpy((char *) wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    strncpy((char *) wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for Wi-Fi connection...");
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to SSID:%s", ssid);
        initialise_sntp();
        return true;
    }

    ESP_LOGE(TAG, "Failed to connect to SSID:%s", ssid);
    return false;
}

void
net_manager_start(void)
{
    if (!s_wifi_initialised) {
        ESP_LOGE(TAG, "net_manager_start called before init");
        return;
    }

    wifi_connect_with_credentials(SER2NET_WIFI_SSID, SER2NET_WIFI_PASSWORD);
}

void
net_manager_stop(void)
{
    if (!s_wifi_initialised)
        return;

    ESP_LOGI(TAG, "Stopping network manager");
    sntp_stop();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (handler_any_id) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, handler_any_id);
        handler_any_id = NULL;
    }
    if (handler_got_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, handler_got_ip);
        handler_got_ip = NULL;
    }
    if (wifi_event_group) {
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
    }
    s_wifi_initialised = false;
    s_retry_num = 0;
}
