#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <inttypes.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/uart.h"

#include "runtime.h"
#include "config.h"
#include "json_config.h"
#include "adapters.h"
#include "config_store.h"

#include "net_manager.h"
#include "web_server.h"

static const char *TAG = "ser2net_main";

#include "wifi_config.h"

static const char config_json[] =
#include "config.json"
;

struct persist_context {
    uint16_t control_port;
    int control_backlog;
};

static struct persist_context s_persist_ctx = {0};

static void persist_runtime_snapshot(void *ctx)
{
    struct persist_context *persist = (struct persist_context *) ctx;
    struct ser2net_esp32_serial_port_cfg ports[SER2NET_MAX_PORTS];
    size_t count = ser2net_runtime_copy_ports(ports, SER2NET_MAX_PORTS);
    if (!config_store_save_ports(ports, count)) {
        ESP_LOGW(TAG, "Failed to persist serial port configuration");
    }
    if (persist && !config_store_save_control(persist->control_port, persist->control_backlog)) {
        ESP_LOGW(TAG, "Failed to persist control configuration");
    }
}

static bool rebuild_runtime_serial(struct ser2net_app_config *app_cfg,
                                   struct ser2net_esp32_serial_cfg *serial_cfg,
                                   const struct ser2net_esp32_network_cfg *net_cfg)
{
    if (!app_cfg || !serial_cfg || !net_cfg)
        return false;

    for (size_t i = 0; i < app_cfg->runtime_cfg.listener_count; ++i) {
        if (app_cfg->runtime_cfg.listeners[i].network)
            ser2net_esp32_release_network_if(app_cfg->runtime_cfg.listeners[i].network);
        app_cfg->runtime_cfg.listeners[i].network = NULL;
    }

    app_cfg->runtime_cfg.listener_count = 0;
    app_cfg->network_if = NULL;

    size_t count = serial_cfg->num_ports;
    if (count > SER2NET_MAX_PORTS)
        count = SER2NET_MAX_PORTS;
    app_cfg->session_cfg.port_count = count;

    for (size_t i = 0; i < count; ++i) {
        const struct ser2net_esp32_serial_port_cfg *p = &serial_cfg->ports[i];
        app_cfg->session_cfg.port_ids[i] = p->port_id;
        app_cfg->session_cfg.tcp_ports[i] = p->tcp_port;
        app_cfg->session_cfg.port_modes[i] = p->mode;

        app_cfg->session_cfg.port_params[i].baud = p->baud_rate;
        switch (p->data_bits) {
        case UART_DATA_5_BITS: app_cfg->session_cfg.port_params[i].data_bits = 5; break;
        case UART_DATA_6_BITS: app_cfg->session_cfg.port_params[i].data_bits = 6; break;
        case UART_DATA_7_BITS: app_cfg->session_cfg.port_params[i].data_bits = 7; break;
        default: app_cfg->session_cfg.port_params[i].data_bits = 8; break;
        }
        if (p->parity == UART_PARITY_ODD)
            app_cfg->session_cfg.port_params[i].parity = 1;
        else if (p->parity == UART_PARITY_EVEN)
            app_cfg->session_cfg.port_params[i].parity = 2;
        else
            app_cfg->session_cfg.port_params[i].parity = 0;

        switch (p->stop_bits) {
        case UART_STOP_BITS_2: app_cfg->session_cfg.port_params[i].stop_bits = 2; break;
#ifdef UART_STOP_BITS_1_5
        case UART_STOP_BITS_1_5: app_cfg->session_cfg.port_params[i].stop_bits = 15; break;
#endif
        default: app_cfg->session_cfg.port_params[i].stop_bits = 1; break;
        }

        app_cfg->session_cfg.port_params[i].flow_control =
            (p->flow_ctrl == UART_HW_FLOWCTRL_CTS_RTS) ? 1 : 0;
        app_cfg->session_cfg.idle_timeout_ms[i] = p->idle_timeout_ms;

        struct ser2net_esp32_network_cfg listener_cfg = {
            .listen_port = p->tcp_port,
            .backlog = p->tcp_backlog > 0 ? p->tcp_backlog :
                        (net_cfg->backlog > 0 ? net_cfg->backlog : 4)
        };

        const struct ser2net_network_if *net_if = ser2net_esp32_get_network_if(&listener_cfg);
        if (!net_if) {
            ESP_LOGE(TAG, "Failed to acquire network listener for TCP %u", p->tcp_port);
            return false;
        }

        app_cfg->runtime_cfg.listeners[app_cfg->runtime_cfg.listener_count].port_id = p->port_id;
        app_cfg->runtime_cfg.listeners[app_cfg->runtime_cfg.listener_count].tcp_port = p->tcp_port;
        app_cfg->runtime_cfg.listeners[app_cfg->runtime_cfg.listener_count].network = net_if;
        if (app_cfg->runtime_cfg.listener_count == 0)
            app_cfg->network_if = net_if;
        app_cfg->runtime_cfg.listener_count++;
    }

    return true;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    if (!net_manager_init()) {
        ESP_LOGE(TAG, "Network manager init failed");
        return;
    }

    net_manager_start();

    if (!web_server_start()) {
        ESP_LOGE(TAG, "Web server failed to start");
    }

    struct ser2net_app_config app_cfg = {0};
    struct ser2net_esp32_network_cfg net_cfg = {0};
    struct ser2net_esp32_serial_cfg serial_cfg = {0};
    struct ser2net_esp32_serial_port_cfg serial_ports[SER2NET_MAX_PORTS];
    memset(serial_ports, 0, sizeof(serial_ports));

    ESP_LOGI(TAG, "Loading configuration (%zu bytes)", strlen(config_json));
    if (ser2net_load_config_json_esp32(config_json,
                                       &app_cfg,
                                       &net_cfg,
                                       &serial_cfg,
                                       serial_ports,
                                       sizeof(serial_ports) / sizeof(serial_ports[0])) != pdPASS) {
        const char *err = ser2net_json_last_error();
        ESP_LOGE(TAG, "Config load failed: %s", err ? err : "unknown");
        return;
    }

    uint16_t stored_control_port = 0;
    int stored_control_backlog = 0;
    if (config_store_load_control(&stored_control_port, &stored_control_backlog)) {
        if (stored_control_port > 0) {
            app_cfg.runtime_cfg.control_enabled = true;
            app_cfg.runtime_cfg.control_ctx.tcp_port = stored_control_port;
        }
        if (stored_control_backlog > 0)
            app_cfg.runtime_cfg.control_ctx.backlog = stored_control_backlog;
    }

    struct ser2net_esp32_serial_port_cfg default_ports[SER2NET_MAX_PORTS];
    memcpy(default_ports, serial_ports, sizeof(default_ports));

    size_t stored_port_count = 0;
    struct ser2net_esp32_serial_port_cfg persisted_ports[SER2NET_MAX_PORTS];
    bool have_persisted_ports = config_store_load_ports(persisted_ports, SER2NET_MAX_PORTS, &stored_port_count);
    size_t default_port_count = serial_cfg.num_ports;
    if (have_persisted_ports) {
        if (stored_port_count > SER2NET_MAX_PORTS)
            stored_port_count = SER2NET_MAX_PORTS;
        memcpy(serial_ports, persisted_ports, stored_port_count * sizeof(struct ser2net_esp32_serial_port_cfg));
        serial_cfg.num_ports = stored_port_count;
        if (!rebuild_runtime_serial(&app_cfg, &serial_cfg, &net_cfg)) {
            ESP_LOGE(TAG, "Failed to rebuild runtime from persisted ports, falling back to static config");
            memcpy(serial_ports, default_ports, sizeof(default_ports));
            serial_cfg.num_ports = default_port_count;
            rebuild_runtime_serial(&app_cfg, &serial_cfg, &net_cfg);
        }
        app_cfg.runtime_cfg.control_ctx.ports = serial_cfg.ports;
        app_cfg.runtime_cfg.control_ctx.port_count = serial_cfg.num_ports;
    }

    app_cfg.runtime_cfg.control_ctx.ports = serial_cfg.ports;
    app_cfg.runtime_cfg.control_ctx.port_count = serial_cfg.num_ports;

    s_persist_ctx.control_port = app_cfg.runtime_cfg.control_ctx.tcp_port;
    s_persist_ctx.control_backlog = app_cfg.runtime_cfg.control_ctx.backlog;
    (void) config_store_save_control(s_persist_ctx.control_port, s_persist_ctx.control_backlog);

    app_cfg.runtime_cfg.config_changed_cb = persist_runtime_snapshot;
    app_cfg.runtime_cfg.config_changed_ctx = &s_persist_ctx;

    if (ser2net_start(&app_cfg) != pdPASS) {
        ESP_LOGE(TAG, "ser2net_start() failed");
        return;
    }

    persist_runtime_snapshot(&s_persist_ctx);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
