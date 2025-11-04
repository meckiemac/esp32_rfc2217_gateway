#include "config_store.h"
#include "adapters.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "config_store";

#define STORE_NAMESPACE      "ser2net"
#define KEY_PORTS_VERSION    "ports_ver"
#define KEY_PORTS_COUNT      "ports_count"
#define KEY_PORTS_BLOB       "ports_blob"
#define KEY_CONTROL_PORT     "ctrl_port"
#define KEY_CONTROL_BACKLOG  "ctrl_backlog"
#define KEY_WIFI_SSID        "wifi_ssid"
#define KEY_WIFI_PASSWORD    "wifi_pass"
#define KEY_WIFI_AP_FORCE_OFF "wifi_ap_force"
#define PORTS_STORE_VERSION  1

static inline bool is_success(esp_err_t err)
{
    return err == ESP_OK;
}

bool config_store_load_ports(struct ser2net_esp32_serial_port_cfg *ports,
                             size_t max_ports,
                             size_t *out_count)
{
    if (!ports || max_ports == 0 || !out_count)
        return false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
        return false;

    uint8_t stored_version = 0;
    err = nvs_get_u8(handle, KEY_PORTS_VERSION, &stored_version);
    if (err != ESP_OK || stored_version != PORTS_STORE_VERSION) {
        nvs_close(handle);
        return false;
    }

    uint32_t stored_count = 0;
    err = nvs_get_u32(handle, KEY_PORTS_COUNT, &stored_count);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    *out_count = stored_count;
    if (stored_count == 0) {
        nvs_close(handle);
        return true;
    }

    size_t blob_size = 0;
    err = nvs_get_blob(handle, KEY_PORTS_BLOB, NULL, &blob_size);
    if (err != ESP_OK || blob_size != stored_count * sizeof(struct ser2net_esp32_serial_port_cfg)) {
        nvs_close(handle);
        ESP_LOGW(TAG, "Stored port blob has unexpected size");
        return false;
    }

    struct ser2net_esp32_serial_port_cfg *buffer =
        malloc(blob_size);
    if (!buffer) {
        nvs_close(handle);
        ESP_LOGE(TAG, "Out of memory reading port blob");
        return false;
    }

    err = nvs_get_blob(handle, KEY_PORTS_BLOB, buffer, &blob_size);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load port blob: %s", esp_err_to_name(err));
        free(buffer);
        return false;
    }

    size_t to_copy = stored_count;
    if (to_copy > max_ports)
        to_copy = max_ports;
    memcpy(ports, buffer, to_copy * sizeof(struct ser2net_esp32_serial_port_cfg));
    free(buffer);
    *out_count = to_copy;

    if (stored_count > max_ports) {
        ESP_LOGW(TAG, "Stored port count (%" PRIu32 ") exceeds buffer capacity (%zu), truncating.",
                 stored_count, max_ports);
    }

    return true;
}

bool config_store_save_ports(const struct ser2net_esp32_serial_port_cfg *ports,
                             size_t count)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return false;

    err = nvs_set_u8(handle, KEY_PORTS_VERSION, PORTS_STORE_VERSION);
    if (err == ESP_OK)
        err = nvs_set_u32(handle, KEY_PORTS_COUNT, (uint32_t) count);

    if (err == ESP_OK) {
        if (count > 0) {
            err = nvs_set_blob(handle, KEY_PORTS_BLOB,
                               ports, count * sizeof(struct ser2net_esp32_serial_port_cfg));
        } else {
            esp_err_t erase_err = nvs_erase_key(handle, KEY_PORTS_BLOB);
            if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND)
                err = erase_err;
        }
    }

    if (err == ESP_OK)
        err = nvs_commit(handle);

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist ports: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool config_store_load_control(uint16_t *tcp_port, int *backlog)
{
    if (!tcp_port || !backlog)
        return false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
        return false;

    uint16_t stored_port = 0;
    err = nvs_get_u16(handle, KEY_CONTROL_PORT, &stored_port);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    int32_t stored_backlog = 0;
    err = nvs_get_i32(handle, KEY_CONTROL_BACKLOG, &stored_backlog);
    nvs_close(handle);
    if (err != ESP_OK)
        return false;

    *tcp_port = stored_port;
    *backlog = (int) stored_backlog;
    return true;
}

bool config_store_save_control(uint16_t tcp_port, int backlog)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return false;

    err = nvs_set_u16(handle, KEY_CONTROL_PORT, tcp_port);
    if (err == ESP_OK)
        err = nvs_set_i32(handle, KEY_CONTROL_BACKLOG, backlog);
    if (err == ESP_OK)
        err = nvs_commit(handle);

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist control config: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void config_store_clear_ports(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return;

    nvs_erase_key(handle, KEY_PORTS_VERSION);
    nvs_erase_key(handle, KEY_PORTS_COUNT);
    nvs_erase_key(handle, KEY_PORTS_BLOB);
    nvs_commit(handle);
    nvs_close(handle);
}

bool config_store_load_wifi_credentials(char *ssid,
                                        size_t ssid_len,
                                        char *password,
                                        size_t password_len)
{
    if (!ssid || !password || ssid_len == 0 || password_len == 0)
        return false;

    nvs_handle_t handle;
    if (nvs_open(STORE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return false;

    size_t len = 0;
    esp_err_t err = nvs_get_str(handle, KEY_WIFI_SSID, NULL, &len);
    if (err != ESP_OK || len == 0 || len > ssid_len) {
        nvs_close(handle);
        return false;
    }
    err = nvs_get_str(handle, KEY_WIFI_SSID, ssid, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    len = 0;
    err = nvs_get_str(handle, KEY_WIFI_PASSWORD, NULL, &len);
    if (err != ESP_OK || len > password_len) {
        nvs_close(handle);
        return false;
    }
    err = nvs_get_str(handle, KEY_WIFI_PASSWORD, password, &len);
    nvs_close(handle);
    if (err != ESP_OK)
        return false;

    return true;
}

bool config_store_save_wifi_credentials(const char *ssid, const char *password)
{
    if (!ssid)
        return false;

    if (!password)
        password = "";

    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return false;

    err = nvs_set_str(handle, KEY_WIFI_SSID, ssid);
    if (err == ESP_OK)
        err = nvs_set_str(handle, KEY_WIFI_PASSWORD, password);
    if (err == ESP_OK)
        err = nvs_commit(handle);

    nvs_close(handle);
    return err == ESP_OK;
}

void config_store_clear_wifi_credentials(void)
{
    nvs_handle_t handle;
    if (nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
        return;

    nvs_erase_key(handle, KEY_WIFI_SSID);
    nvs_erase_key(handle, KEY_WIFI_PASSWORD);
    nvs_commit(handle);
    nvs_close(handle);
}

bool config_store_load_softap_forced_disable(bool *forced_disable)
{
    if (!forced_disable)
        return false;

    nvs_handle_t handle;
    if (nvs_open(STORE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
        return false;

    uint8_t value = 0;
    esp_err_t err = nvs_get_u8(handle, KEY_WIFI_AP_FORCE_OFF, &value);
    nvs_close(handle);
    if (err != ESP_OK)
        return false;

    *forced_disable = value != 0;
    return true;
}

bool config_store_save_softap_forced_disable(bool forced_disable)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
        return false;

    err = nvs_set_u8(handle, KEY_WIFI_AP_FORCE_OFF, forced_disable ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_commit(handle);

    nvs_close(handle);
    return err == ESP_OK;
}
