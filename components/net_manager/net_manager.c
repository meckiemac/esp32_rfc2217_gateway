#include "net_manager.h"

#include "config_store.h"
#include "wifi_config.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_netif_ip_addr.h>
#include <lwip/apps/sntp.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include <string.h>

static const char *TAG = "net_manager";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAXIMUM_RETRY      5

#define SOFTAP_TIMEOUT_MS  (SER2NET_AP_ACTIVE_TIMEOUT_SEC * 1000U)

static EventGroupHandle_t wifi_event_group;
static int s_retry_num;
static bool s_wifi_initialised;
static bool s_wifi_started;
static bool s_sta_configured;
static bool s_sta_connected;
static bool s_ap_running;
static bool s_ap_force_disable;
static uint32_t s_ap_timeout_ms = SOFTAP_TIMEOUT_MS;

static esp_event_handler_instance_t handler_any_id;
static esp_event_handler_instance_t handler_got_ip;
static esp_netif_t *s_netif_sta;
static esp_netif_t *s_netif_ap;
static TimerHandle_t s_ap_timer;

static wifi_config_t s_sta_config;
static char s_sta_ssid[33];
static char s_sta_ip[16];

static void initialise_sntp(void);
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void softap_timeout_callback(TimerHandle_t timer);
static void fill_ap_config(wifi_config_t *cfg);
static void update_sta_credentials(const char *ssid, const char *password);
static void ensure_softap_running(void);
static void stop_softap(void);
static bool connect_station(bool wait_for_ip);

static void initialise_sntp(void)
{
    ESP_LOGI(TAG, "Initialising SNTP");
    sntp_stop();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_sta_configured)
            esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *) event_data;
        s_sta_connected = false;
        if (!s_sta_configured) {
            return;
        }

        if (disc && disc->reason == WIFI_REASON_ASSOC_LEAVE) {
            ESP_LOGI(TAG, "Station disconnect requested, not retrying");
            return;
        }

        ESP_LOGW(TAG, "Disconnected from AP (reason %d)", disc ? disc->reason : -1);
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "Retrying Wi-Fi connection (%d/%d)", s_retry_num, MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        s_ap_running = true;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        s_ap_running = false;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Obtained IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_sta_connected = true;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_sta_ip, sizeof(s_sta_ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        initialise_sntp();
    }
}

static void softap_timeout_callback(TimerHandle_t timer)
{
    (void) timer;
    if (!s_wifi_started || !s_sta_configured || s_ap_force_disable)
        return;

    ESP_LOGI(TAG, "SoftAP timeout reached, disabling AP interface");
    stop_softap();
}

static void ensure_timer_created(void)
{
    if (!s_ap_timer && s_ap_timeout_ms > 0) {
        s_ap_timer = xTimerCreate("ap_timeout",
                                  pdMS_TO_TICKS(s_ap_timeout_ms),
                                  pdFALSE,
                                  NULL,
                                  softap_timeout_callback);
    }
}

static void start_softap_timer(void)
{
    if (!s_sta_configured || s_ap_force_disable || s_ap_timeout_ms == 0)
        return;
    ensure_timer_created();
    if (s_ap_timer)
        xTimerStart(s_ap_timer, 0);
}

static void stop_softap_timer(void)
{
    if (s_ap_timer)
        xTimerStop(s_ap_timer, 0);
}

static void fill_ap_config(wifi_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy((char *) cfg->ap.ssid, SER2NET_AP_SSID, sizeof(cfg->ap.ssid) - 1);
    cfg->ap.ssid_len = strlen((const char *) cfg->ap.ssid);
    cfg->ap.channel = SER2NET_AP_CHANNEL;
    cfg->ap.max_connection = SER2NET_AP_MAX_CLIENTS;

    size_t pass_len = strlen(SER2NET_AP_PASSWORD);
    if (pass_len > 0) {
        strncpy((char *) cfg->ap.password, SER2NET_AP_PASSWORD, sizeof(cfg->ap.password) - 1);
        cfg->ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    } else {
        cfg->ap.authmode = WIFI_AUTH_OPEN;
    }
}

static void update_sta_credentials(const char *ssid, const char *password)
{
    memset(&s_sta_config, 0, sizeof(s_sta_config));
    strncpy((char *) s_sta_config.sta.ssid, ssid, sizeof(s_sta_config.sta.ssid) - 1);
    strncpy((char *) s_sta_config.sta.password, password ? password : "", sizeof(s_sta_config.sta.password) - 1);
    s_sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    strncpy(s_sta_ssid, ssid, sizeof(s_sta_ssid) - 1);
    s_sta_ssid[sizeof(s_sta_ssid) - 1] = '\0';
}

static void ensure_softap_running(void)
{
    if (s_ap_force_disable)
        return;

    wifi_config_t ap_config;
    fill_ap_config(&ap_config);

    ESP_ERROR_CHECK(esp_wifi_set_mode(s_sta_configured ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    s_ap_running = true;
}

static void stop_softap(void)
{
    if (!s_wifi_started)
        return;

    stop_softap_timer();
    if (!s_ap_running)
        return;

    if (!s_sta_configured) {
        ESP_LOGW(TAG, "Ignoring request to disable SoftAP while station is unconfigured");
        return;
    }

    ESP_LOGI(TAG, "Disabling SoftAP interface");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_ap_running = false;
}

static bool connect_station(bool wait_for_ip)
{
    if (!s_sta_configured)
        return false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &s_sta_config));
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;
    esp_wifi_connect();

    if (!wait_for_ip)
        return true;

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT)
        return true;

    if (bits & WIFI_FAIL_BIT)
        ESP_LOGE(TAG, "Failed to connect to SSID:%s", s_sta_ssid);
    else
        ESP_LOGW(TAG, "Timed out waiting for Wi-Fi connection");
    return false;
}

bool net_manager_init(void)
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

    s_netif_sta = esp_netif_create_default_wifi_sta();
    s_netif_ap = esp_netif_create_default_wifi_ap();
    if (!s_netif_sta || !s_netif_ap) {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi netifs");
        vEventGroupDelete(wifi_event_group);
        wifi_event_group = NULL;
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
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

    bool forced_disable = false;
    if (config_store_load_softap_forced_disable(&forced_disable))
        s_ap_force_disable = forced_disable;

    s_retry_num = 0;
    s_sta_configured = false;
    s_sta_connected = false;
    s_ap_running = false;
    s_sta_ip[0] = '\0';
    s_wifi_initialised = true;
    ESP_LOGI(TAG, "Network manager initialised");
    return true;
}

void net_manager_start(void)
{
    if (!s_wifi_initialised) {
        ESP_LOGE(TAG, "net_manager_start called before init");
        return;
    }

    char ssid[33] = {0};
    char password[65] = {0};
    bool have_creds = config_store_load_wifi_credentials(ssid, sizeof(ssid), password, sizeof(password));

    if (!have_creds && strlen(SER2NET_WIFI_SSID) > 0) {
        strncpy(ssid, SER2NET_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(password, SER2NET_WIFI_PASSWORD, sizeof(password) - 1);
        have_creds = true;
    }

    if (!have_creds) {
        ESP_LOGW(TAG, "No Wi-Fi credentials; enabling provisioning SoftAP");
        s_sta_configured = false;
        s_ap_force_disable = false; /* ignore previous forced state to allow provisioning */
    } else {
        s_sta_configured = true;
        update_sta_credentials(ssid, password);
    }
    s_sta_ip[0] = '\0';

    wifi_config_t ap_config;
    fill_ap_config(&ap_config);

    if (s_wifi_started)
        esp_wifi_stop();

    if (!s_sta_configured) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        s_ap_running = true;
    } else if (s_ap_force_disable) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        s_ap_running = false;
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        s_ap_running = true;
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;

    if (s_sta_configured) {
        if (!connect_station(true)) {
            ESP_LOGW(TAG, "Station connection failed; keeping SoftAP active");
        }
    } else {
        s_sta_connected = false;
        s_sta_ssid[0] = '\0';
    }

    if (!s_ap_force_disable && s_sta_configured)
        start_softap_timer();
    else
        stop_softap_timer();
}

void net_manager_stop(void)
{
    if (!s_wifi_initialised)
        return;

    ESP_LOGI(TAG, "Stopping network manager");
    stop_softap_timer();
    sntp_stop();
    esp_wifi_stop();
    esp_wifi_deinit();
    s_wifi_started = false;
    s_ap_running = false;

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

bool net_manager_apply_credentials(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0])
        return false;

    if (!config_store_save_wifi_credentials(ssid, password ? password : ""))
        return false;

    s_sta_configured = true;
    update_sta_credentials(ssid, password);
    s_sta_ip[0] = '\0';
    s_sta_connected = false;
    s_ap_force_disable = false;
    config_store_save_softap_forced_disable(false);

    if (!s_wifi_started) {
        net_manager_start();
        return true;
    }

    ensure_softap_running();
    stop_softap_timer();
    connect_station(false);
    start_softap_timer();
    return true;
}

void net_manager_forget_credentials(void)
{
    config_store_clear_wifi_credentials();
    s_sta_configured = false;
    s_sta_connected = false;
    s_sta_ssid[0] = '\0';
    s_sta_ip[0] = '\0';

    if (!s_wifi_started)
        return;

    stop_softap_timer();
    esp_wifi_disconnect();
    s_ap_force_disable = false;
    config_store_save_softap_forced_disable(false);

    wifi_config_t ap_config;
    fill_ap_config(&ap_config);
    esp_wifi_set_mode(WIFI_MODE_AP);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    s_ap_running = true;
}

bool net_manager_set_softap_forced_disable(bool forced_disable)
{
    if (forced_disable && !s_sta_configured) {
        ESP_LOGW(TAG, "Ignoring request to disable SoftAP without STA credentials");
        return false;
    }

    if (!config_store_save_softap_forced_disable(forced_disable))
        return false;

    s_ap_force_disable = forced_disable;

    if (!s_wifi_started)
        return true;

    if (forced_disable) {
        stop_softap();
    } else {
        ensure_softap_running();
        start_softap_timer();
    }

    return true;
}

bool net_manager_get_status(struct net_manager_status *status)
{
    if (!status)
        return false;

    status->sta_configured = s_sta_configured;
    status->sta_connected = s_sta_connected;
    strncpy(status->sta_ssid, s_sta_ssid, sizeof(status->sta_ssid) - 1);
    status->sta_ssid[sizeof(status->sta_ssid) - 1] = '\0';
    strncpy(status->sta_ip, s_sta_ip, sizeof(status->sta_ip) - 1);
    status->sta_ip[sizeof(status->sta_ip) - 1] = '\0';
    status->ap_active = s_ap_running;
    status->ap_force_disabled = s_ap_force_disable;

    if (s_ap_timer && xTimerIsTimerActive(s_ap_timer)) {
        TickType_t expires = xTimerGetExpiryTime(s_ap_timer);
        TickType_t now = xTaskGetTickCount();
        if (expires > now)
            status->ap_remaining_seconds = (uint32_t) (((expires - now) * portTICK_PERIOD_MS) / 1000U);
        else
            status->ap_remaining_seconds = 0;
    } else {
        status->ap_remaining_seconds = 0;
    }
    return true;
}
