#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"

#include <cJSON.h>
#include <driver/uart.h>
#include "esp_log.h"
#include <esp_http_server.h>

#include "pinmap.h"
#include "runtime.h"
#include "session_ops.h"
#include "control_port.h"
#include "adapters.h"

static const char *TAG = "web_server";
static httpd_handle_t s_server = NULL;

#define MAX_REQUEST_BODY 1024

static const char *port_mode_to_str(enum ser2net_port_mode mode)
{
    switch (mode) {
    case SER2NET_PORT_MODE_RAW: return "raw";
    case SER2NET_PORT_MODE_RAWLP: return "rawlp";
    case SER2NET_PORT_MODE_TELNET:
    default: return "telnet";
    }
}

static enum ser2net_port_mode port_mode_from_str(const char *str)
{
    if (!str)
        return SER2NET_PORT_MODE_TELNET;
    if (strcasecmp(str, "raw") == 0)
        return SER2NET_PORT_MODE_RAW;
    if (strcasecmp(str, "rawlp") == 0)
        return SER2NET_PORT_MODE_RAWLP;
    return SER2NET_PORT_MODE_TELNET;
}

static int data_bits_to_int(uart_word_length_t len)
{
    switch (len) {
    case UART_DATA_5_BITS: return 5;
    case UART_DATA_6_BITS: return 6;
    case UART_DATA_7_BITS: return 7;
    case UART_DATA_8_BITS:
    default:
        return 8;
    }
}

static double stop_bits_to_value(uart_stop_bits_t stop)
{
    switch (stop) {
    case UART_STOP_BITS_2: return 2.0;
#ifdef UART_STOP_BITS_1_5
    case UART_STOP_BITS_1_5: return 1.5;
#endif
    case UART_STOP_BITS_1:
    default:
        return 1.0;
    }
}

static const char *parity_to_str(uart_parity_t parity)
{
    switch (parity) {
    case UART_PARITY_ODD: return "odd";
    case UART_PARITY_EVEN: return "even";
    case UART_PARITY_DISABLE:
    default:
        return "none";
    }
}

static int sessions_for_port(int port_id,
                             const struct ser2net_active_session *sessions,
                             size_t count)
{
    int total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (sessions[i].port_id == port_id)
            total++;
    }
    return total;
}

static esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message)
{
    if (status)
        httpd_resp_set_status(req, status);
    if (!message)
        message = "error";

    char buf[128];
    int written = snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", message);
    if (written < 0 || written >= (int)sizeof(buf))
        return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static bool read_json_body(httpd_req_t *req, cJSON **out_json)
{
    if (!out_json)
        return false;

    if (req->content_len <= 0) {
        send_json_error(req, "400 Bad Request", "body required");
        return false;
    }
    if (req->content_len > MAX_REQUEST_BODY) {
        send_json_error(req, "413 Payload Too Large", "request too large");
        return false;
    }

    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return false;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_500(req);
            return false;
        }
        received += ret;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        send_json_error(req, "400 Bad Request", "invalid json");
        return false;
    }

    *out_json = root;
    return true;
}

static const struct ser2net_esp32_serial_port_cfg *
find_port_by_tcp(uint16_t tcp_port,
                 const struct ser2net_esp32_serial_port_cfg *ports,
                 size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (ports[i].tcp_port == tcp_port)
            return &ports[i];
    }
    return NULL;
}

static void fill_params_from_cfg(const struct ser2net_esp32_serial_port_cfg *cfg,
                                 struct ser2net_serial_params *params)
{
    if (!cfg || !params)
        return;
    params->baud = cfg->baud_rate;
    switch (cfg->data_bits) {
    case UART_DATA_5_BITS: params->data_bits = 5; break;
    case UART_DATA_6_BITS: params->data_bits = 6; break;
    case UART_DATA_7_BITS: params->data_bits = 7; break;
    default: params->data_bits = 8; break;
    }
    if (cfg->parity == UART_PARITY_ODD)
        params->parity = 1;
    else if (cfg->parity == UART_PARITY_EVEN)
        params->parity = 2;
    else
        params->parity = 0;

    switch (cfg->stop_bits) {
    case UART_STOP_BITS_2: params->stop_bits = 2; break;
#ifdef UART_STOP_BITS_1_5
    case UART_STOP_BITS_1_5: params->stop_bits = 15; break;
#endif
    default: params->stop_bits = 1; break;
    }

    params->flow_control = (cfg->flow_ctrl == UART_HW_FLOWCTRL_CTS_RTS) ? 1 : 0;
}

static cJSON *port_to_json(const struct ser2net_esp32_serial_port_cfg *cfg, int active_sessions)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return NULL;

    cJSON_AddNumberToObject(obj, "port_id", cfg->port_id);
    cJSON_AddNumberToObject(obj, "tcp_port", cfg->tcp_port);
    cJSON_AddNumberToObject(obj, "uart", cfg->uart_num);
    cJSON_AddNumberToObject(obj, "tx_pin", cfg->tx_pin);
    cJSON_AddNumberToObject(obj, "rx_pin", cfg->rx_pin);
    if (cfg->rts_pin != UART_PIN_NO_CHANGE)
        cJSON_AddNumberToObject(obj, "rts_pin", cfg->rts_pin);
    if (cfg->cts_pin != UART_PIN_NO_CHANGE)
        cJSON_AddNumberToObject(obj, "cts_pin", cfg->cts_pin);
    cJSON_AddStringToObject(obj, "mode", port_mode_to_str(cfg->mode));
    cJSON_AddBoolToObject(obj, "enabled", cfg->enabled);
    cJSON_AddNumberToObject(obj, "baud", cfg->baud_rate);
    cJSON_AddNumberToObject(obj, "data_bits", data_bits_to_int(cfg->data_bits));
    cJSON_AddStringToObject(obj, "parity", parity_to_str(cfg->parity));
    cJSON_AddNumberToObject(obj, "stop_bits", stop_bits_to_value(cfg->stop_bits));
    cJSON_AddNumberToObject(obj, "flow_control", cfg->flow_ctrl);
    cJSON_AddNumberToObject(obj, "idle_timeout_ms", cfg->idle_timeout_ms);
    cJSON_AddNumberToObject(obj, "active_sessions", active_sessions);
    if (cfg->pin_map[0] != '\0')
        cJSON_AddStringToObject(obj, "pin_map", cfg->pin_map);
    return obj;
}

static esp_err_t send_json_response(httpd_req_t *req, cJSON *json, int status)
{
    char *payload = cJSON_PrintUnformatted(json);
    if (!payload)
        return httpd_resp_send_500(req);

    if (status == 201)
        httpd_resp_set_status(req, "201 Created");
    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_send(req, payload, HTTPD_RESP_USE_STRLEN);
    cJSON_free(payload);
    return res;
}

static esp_err_t health_handler(httpd_req_t *req)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj)
        return httpd_resp_send_500(req);
    cJSON_AddStringToObject(obj, "status", "ok");
    esp_err_t res = send_json_response(req, obj, 200);
    cJSON_Delete(obj);
    return res;
}

static esp_err_t ports_get_handler(httpd_req_t *req)
{
    struct ser2net_esp32_serial_port_cfg ports[SER2NET_MAX_PORTS];
    size_t count = ser2net_runtime_copy_ports(ports, SER2NET_MAX_PORTS);

    struct ser2net_active_session sessions[SER2NET_MAX_PORTS];
    size_t session_count = ser2net_runtime_list_sessions(sessions, SER2NET_MAX_PORTS);

    cJSON *root = cJSON_CreateArray();
    if (!root)
        return httpd_resp_send_500(req);

    for (size_t i = 0; i < count; ++i) {
        int active = sessions_for_port(ports[i].port_id, sessions, session_count);
        cJSON *item = port_to_json(&ports[i], active);
        if (!item) {
            cJSON_Delete(root);
            return httpd_resp_send_500(req);
        }
        cJSON_AddItemToArray(root, item);
    }

    esp_err_t res = send_json_response(req, root, 200);
    cJSON_Delete(root);
    return res;
}

static bool parse_port_config(cJSON *root, struct ser2net_esp32_serial_port_cfg *cfg)
{
    cJSON *port_id = cJSON_GetObjectItem(root, "port_id");
    cJSON *tcp_port = cJSON_GetObjectItem(root, "tcp_port");
    cJSON *uart = cJSON_GetObjectItem(root, "uart");
    cJSON *tx_pin = cJSON_GetObjectItem(root, "tx_pin");
    cJSON *rx_pin = cJSON_GetObjectItem(root, "rx_pin");

    if (!cJSON_IsNumber(port_id) || !cJSON_IsNumber(tcp_port) ||
        !cJSON_IsNumber(uart) || !cJSON_IsNumber(tx_pin) || !cJSON_IsNumber(rx_pin))
        return false;

    cfg->port_id = port_id->valueint;
    cfg->tcp_port = (uint16_t) tcp_port->valueint;
    cfg->uart_num = (uart_port_t) uart->valueint;
    cfg->tx_pin = tx_pin->valueint;
    cfg->rx_pin = rx_pin->valueint;

    cJSON *rts = cJSON_GetObjectItem(root, "rts_pin");
    if (cJSON_IsNumber(rts))
        cfg->rts_pin = rts->valueint;

    cJSON *cts = cJSON_GetObjectItem(root, "cts_pin");
    if (cJSON_IsNumber(cts))
        cfg->cts_pin = cts->valueint;

    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsString(mode))
        cfg->mode = port_mode_from_str(mode->valuestring);

    cJSON *enabled = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(enabled))
        cfg->enabled = cJSON_IsTrue(enabled);

    cJSON *baud = cJSON_GetObjectItem(root, "baud");
    if (cJSON_IsNumber(baud) && baud->valueint > 0)
        cfg->baud_rate = baud->valueint;

    cJSON *data_bits = cJSON_GetObjectItem(root, "data_bits");
    if (cJSON_IsNumber(data_bits)) {
        switch (data_bits->valueint) {
        case 5: cfg->data_bits = UART_DATA_5_BITS; break;
        case 6: cfg->data_bits = UART_DATA_6_BITS; break;
        case 7: cfg->data_bits = UART_DATA_7_BITS; break;
        default: cfg->data_bits = UART_DATA_8_BITS; break;
        }
    }

    cJSON *parity = cJSON_GetObjectItem(root, "parity");
    if (cJSON_IsString(parity)) {
        if (strcasecmp(parity->valuestring, "odd") == 0)
            cfg->parity = UART_PARITY_ODD;
        else if (strcasecmp(parity->valuestring, "even") == 0)
            cfg->parity = UART_PARITY_EVEN;
        else
            cfg->parity = UART_PARITY_DISABLE;
    }

    cJSON *stop = cJSON_GetObjectItem(root, "stop_bits");
    if (cJSON_IsNumber(stop)) {
        if (stop->valuedouble >= 2.0)
            cfg->stop_bits = UART_STOP_BITS_2;
#ifdef UART_STOP_BITS_1_5
        else if (stop->valuedouble > 1.0 && stop->valuedouble < 2.0)
            cfg->stop_bits = UART_STOP_BITS_1_5;
#endif
        else
            cfg->stop_bits = UART_STOP_BITS_1;
    }

    cJSON *flow = cJSON_GetObjectItem(root, "flow_control");
    if (cJSON_IsString(flow)) {
        if (strcasecmp(flow->valuestring, "rtscts") == 0)
            cfg->flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS;
        else
            cfg->flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    }

    cJSON *idle = cJSON_GetObjectItem(root, "idle_timeout_ms");
    if (cJSON_IsNumber(idle) && idle->valueint >= 0)
        cfg->idle_timeout_ms = idle->valueint;

    cJSON *pinmap = cJSON_GetObjectItem(root, "pin_map");
    if (cJSON_IsString(pinmap) && pinmap->valuestring) {
        struct ser2net_pin_config map = {
            .tx_pin = cfg->tx_pin,
            .rx_pin = cfg->rx_pin,
            .rts_pin = cfg->rts_pin,
            .cts_pin = cfg->cts_pin
        };
        int parsed_uart = cfg->uart_num;
        if (ser2net_parse_pinmap_string(pinmap->valuestring, &parsed_uart, &map)) {
            cfg->uart_num = parsed_uart;
            cfg->tx_pin = map.tx_pin;
            cfg->rx_pin = map.rx_pin;
            cfg->rts_pin = map.rts_pin >= 0 ? map.rts_pin : UART_PIN_NO_CHANGE;
            cfg->cts_pin = map.cts_pin >= 0 ? map.cts_pin : UART_PIN_NO_CHANGE;
        }
    }

    struct ser2net_pin_config summary = {
        .tx_pin = cfg->tx_pin,
        .rx_pin = cfg->rx_pin,
        .rts_pin = cfg->rts_pin,
        .cts_pin = cfg->cts_pin
    };
    ser2net_format_pinmap_string(cfg->pin_map, sizeof(cfg->pin_map), cfg->uart_num, &summary);

    return cfg->port_id >= 0 && cfg->tcp_port > 0 && cfg->uart_num >= 0 &&
           cfg->tx_pin >= 0 && cfg->rx_pin >= 0;
}

static esp_err_t ports_post_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (!read_json_body(req, &root))
        return ESP_OK;

    struct ser2net_esp32_serial_port_cfg cfg = {
        .port_id = -1,
        .uart_num = UART_NUM_MAX,
        .tx_pin = -1,
        .rx_pin = -1,
        .rts_pin = UART_PIN_NO_CHANGE,
        .cts_pin = UART_PIN_NO_CHANGE,
        .tcp_port = 0,
        .tcp_backlog = 4,
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .mode = SER2NET_PORT_MODE_TELNET,
        .idle_timeout_ms = 0,
        .enabled = true
    };
    cfg.pin_map[0] = '\0';

    bool ok = parse_port_config(root, &cfg);
    cJSON_Delete(root);
    if (!ok)
        return send_json_error(req, "400 Bad Request", "invalid port parameters");

    if (ser2net_runtime_add_port(&cfg) != pdPASS) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "{\"error\":\"port exists or invalid\"}", HTTPD_RESP_USE_STRLEN);
    }

    struct ser2net_esp32_serial_port_cfg snapshot[SER2NET_MAX_PORTS];
    size_t count = ser2net_runtime_copy_ports(snapshot, SER2NET_MAX_PORTS);
    struct ser2net_active_session sessions[SER2NET_MAX_PORTS];
    size_t session_count = ser2net_runtime_list_sessions(sessions, SER2NET_MAX_PORTS);

    const struct ser2net_esp32_serial_port_cfg *added = NULL;
    for (size_t i = 0; i < count; ++i) {
        if (snapshot[i].port_id == cfg.port_id) {
            added = &snapshot[i];
            break;
        }
    }

    if (!added)
        return httpd_resp_send_500(req);

    cJSON *resp = port_to_json(added, sessions_for_port(added->port_id, sessions, session_count));
    if (!resp)
        return httpd_resp_send_500(req);

esp_err_t res = send_json_response(req, resp, 201);
    cJSON_Delete(resp);
    return res;
}

static esp_err_t port_config_handler(httpd_req_t *req, uint16_t tcp_port)
{
    struct ser2net_esp32_serial_port_cfg ports[SER2NET_MAX_PORTS];
    size_t count = ser2net_runtime_copy_ports(ports, SER2NET_MAX_PORTS);
    const struct ser2net_esp32_serial_port_cfg *base = find_port_by_tcp(tcp_port, ports, count);
    if (!base)
        return send_json_error(req, "404 Not Found", "port not found");

    cJSON *root = NULL;
    if (!read_json_body(req, &root))
        return ESP_OK;

    struct ser2net_serial_params params;
    fill_params_from_cfg(base, &params);
    uint32_t idle_timeout = base->idle_timeout_ms;
    bool apply_active = false;
    struct ser2net_pin_config pins = {
        .tx_pin = base->tx_pin,
        .rx_pin = base->rx_pin,
        .rts_pin = base->rts_pin,
        .cts_pin = base->cts_pin
    };
    bool pins_updated = false;

    cJSON *baud = cJSON_GetObjectItemCaseSensitive(root, "baud");
    if (cJSON_IsNumber(baud) && baud->valueint > 0)
        params.baud = baud->valueint;

    cJSON *data_bits = cJSON_GetObjectItemCaseSensitive(root, "data_bits");
    if (cJSON_IsNumber(data_bits)) {
        switch (data_bits->valueint) {
        case 5: params.data_bits = 5; break;
        case 6: params.data_bits = 6; break;
        case 7: params.data_bits = 7; break;
        case 8: params.data_bits = 8; break;
        default:
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "data_bits must be 5-8");
        }
    }

    cJSON *parity = cJSON_GetObjectItemCaseSensitive(root, "parity");
    if (cJSON_IsString(parity) && parity->valuestring) {
        if (strcasecmp(parity->valuestring, "odd") == 0)
            params.parity = 1;
        else if (strcasecmp(parity->valuestring, "even") == 0)
            params.parity = 2;
        else if (strcasecmp(parity->valuestring, "none") == 0)
            params.parity = 0;
        else {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "parity must be none/odd/even");
        }
    }

    cJSON *stop_bits = cJSON_GetObjectItemCaseSensitive(root, "stop_bits");
    if (stop_bits) {
        if (cJSON_IsNumber(stop_bits)) {
            double v = stop_bits->valuedouble;
            if (v >= 1.9)
                params.stop_bits = 2;
#ifdef UART_STOP_BITS_1_5
            else if (v > 1.0 && v < 2.0)
                params.stop_bits = 15;
#endif
            else if (v >= 0.9 && v <= 1.1)
                params.stop_bits = 1;
            else {
                cJSON_Delete(root);
                return send_json_error(req, "400 Bad Request", "stop_bits must be 1/1.5/2");
            }
        } else if (cJSON_IsString(stop_bits) && stop_bits->valuestring) {
            if (strcmp(stop_bits->valuestring, "2") == 0)
                params.stop_bits = 2;
#ifdef UART_STOP_BITS_1_5
            else if (strcmp(stop_bits->valuestring, "1.5") == 0)
                params.stop_bits = 15;
#endif
            else if (strcmp(stop_bits->valuestring, "1") == 0)
                params.stop_bits = 1;
            else {
                cJSON_Delete(root);
                return send_json_error(req, "400 Bad Request", "stop_bits must be 1/1.5/2");
            }
        }
    }

    cJSON *flow = cJSON_GetObjectItemCaseSensitive(root, "flow_control");
    if (cJSON_IsString(flow) && flow->valuestring) {
        if (strcasecmp(flow->valuestring, "rtscts") == 0)
            params.flow_control = 1;
        else if (strcasecmp(flow->valuestring, "none") == 0)
            params.flow_control = 0;
        else {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "flow_control must be none or rtscts");
        }
    }

    cJSON *idle = cJSON_GetObjectItemCaseSensitive(root, "idle_timeout_ms");
    if (cJSON_IsNumber(idle)) {
        if (idle->valueint < 0) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "idle_timeout_ms must be >= 0");
        }
        idle_timeout = (uint32_t) idle->valueint;
    }

    cJSON *apply = cJSON_GetObjectItemCaseSensitive(root, "apply_active");
    if (cJSON_IsBool(apply))
        apply_active = cJSON_IsTrue(apply);

    cJSON *pinmap = cJSON_GetObjectItemCaseSensitive(root, "pin_map");
    if (cJSON_IsString(pinmap) && pinmap->valuestring) {
        struct ser2net_pin_config map = pins;
        if (!ser2net_parse_pinmap_string(pinmap->valuestring, NULL, &map)) {
            cJSON_Delete(root);
            return send_json_error(req, "400 Bad Request", "invalid pin_map");
        }
        pins = map;
        pins_updated = true;
    }

    cJSON *tx = cJSON_GetObjectItemCaseSensitive(root, "tx_pin");
    if (cJSON_IsNumber(tx)) {
        pins.tx_pin = tx->valueint;
        pins_updated = true;
    }
    cJSON *rx = cJSON_GetObjectItemCaseSensitive(root, "rx_pin");
    if (cJSON_IsNumber(rx)) {
        pins.rx_pin = rx->valueint;
        pins_updated = true;
    }
    cJSON *rts = cJSON_GetObjectItemCaseSensitive(root, "rts_pin");
    if (cJSON_IsNumber(rts)) {
        pins.rts_pin = rts->valueint;
        pins_updated = true;
    }
    cJSON *cts = cJSON_GetObjectItemCaseSensitive(root, "cts_pin");
    if (cJSON_IsNumber(cts)) {
        pins.cts_pin = cts->valueint;
        pins_updated = true;
    }

    cJSON_Delete(root);

    if (ser2net_runtime_update_serial_config(tcp_port,
                                             &params,
                                             idle_timeout,
                                             apply_active,
                                             pins_updated ? &pins : NULL) != pdPASS)
        return send_json_error(req, "409 Conflict", "unable to update port");

    struct ser2net_active_session sessions[SER2NET_MAX_PORTS];
    size_t session_count = ser2net_runtime_list_sessions(sessions, SER2NET_MAX_PORTS);
    count = ser2net_runtime_copy_ports(ports, SER2NET_MAX_PORTS);
    base = find_port_by_tcp(tcp_port, ports, count);
    if (!base)
        return httpd_resp_send_500(req);

    cJSON *resp = port_to_json(base, sessions_for_port(base->port_id, sessions, session_count));
    if (!resp)
        return httpd_resp_send_500(req);

    esp_err_t res = send_json_response(req, resp, 200);
    cJSON_Delete(resp);
    return res;
}

static esp_err_t port_mode_handler(httpd_req_t *req, uint16_t tcp_port)
{
    struct ser2net_esp32_serial_port_cfg ports[SER2NET_MAX_PORTS];
    size_t count = ser2net_runtime_copy_ports(ports, SER2NET_MAX_PORTS);
    const struct ser2net_esp32_serial_port_cfg *base = find_port_by_tcp(tcp_port, ports, count);
    if (!base)
        return send_json_error(req, "404 Not Found", "port not found");

    cJSON *root = NULL;
    if (!read_json_body(req, &root))
        return ESP_OK;

    enum ser2net_port_mode mode = base->mode;
    bool enabled = base->enabled;
    bool touched = false;

    cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (cJSON_IsString(mode_item) && mode_item->valuestring) {
        mode = port_mode_from_str(mode_item->valuestring);
        touched = true;
    }

    cJSON *enabled_item = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (cJSON_IsBool(enabled_item)) {
        enabled = cJSON_IsTrue(enabled_item);
        touched = true;
    }

    cJSON_Delete(root);

    if (!touched)
        return send_json_error(req, "400 Bad Request", "mode or enabled required");

    if (ser2net_runtime_set_port_mode(tcp_port, mode, enabled) != pdPASS)
        return send_json_error(req, "409 Conflict", "unable to update mode");

    struct ser2net_active_session sessions[SER2NET_MAX_PORTS];
    size_t session_count = ser2net_runtime_list_sessions(sessions, SER2NET_MAX_PORTS);
    count = ser2net_runtime_copy_ports(ports, SER2NET_MAX_PORTS);
    base = find_port_by_tcp(tcp_port, ports, count);
    if (!base)
        return httpd_resp_send_500(req);

    cJSON *resp = port_to_json(base, sessions_for_port(base->port_id, sessions, session_count));
    if (!resp)
        return httpd_resp_send_500(req);
    esp_err_t res = send_json_response(req, resp, 200);
    cJSON_Delete(resp);
    return res;
}

static esp_err_t port_disconnect_handler(httpd_req_t *req, uint16_t tcp_port)
{
    struct ser2net_esp32_serial_port_cfg ports[SER2NET_MAX_PORTS];
    size_t count = ser2net_runtime_copy_ports(ports, SER2NET_MAX_PORTS);
    if (!find_port_by_tcp(tcp_port, ports, count))
        return send_json_error(req, "404 Not Found", "port not found");

    bool disconnected = ser2net_runtime_disconnect_tcp_port(tcp_port);

    char buf[96];
    int written = snprintf(buf, sizeof(buf),
                           "{\"tcp_port\":%u,\"disconnected\":%s}",
                           tcp_port, disconnected ? "true" : "false");
    if (written < 0 || written >= (int)sizeof(buf))
        return httpd_resp_send_500(req);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ports_action_handler(httpd_req_t *req)
{
    static const char *base = "/api/ports/";
    size_t base_len = strlen(base);
    if (strncmp(req->uri, base, base_len) != 0)
        return send_json_error(req, "404 Not Found", "invalid path");

    const char *cursor = req->uri + base_len;
    size_t segment_len = 0;
    while (cursor[segment_len] && cursor[segment_len] != '/' && cursor[segment_len] != '?')
        segment_len++;

    if (segment_len == 0 || segment_len >= 8)
        return send_json_error(req, "400 Bad Request", "invalid tcp port");

    char port_buf[8];
    memcpy(port_buf, cursor, segment_len);
    port_buf[segment_len] = '\0';

    char *endptr = NULL;
    long tcp_val = strtol(port_buf, &endptr, 10);
    if (!endptr || *endptr != '\0' || tcp_val <= 0 || tcp_val > 65535)
        return send_json_error(req, "400 Bad Request", "invalid tcp port");
    uint16_t tcp_port = (uint16_t) tcp_val;

    const char *action = cursor + segment_len;
    while (*action == '/')
        action++;
    size_t action_len = 0;
    while (action[action_len] && action[action_len] != '/' && action[action_len] != '?')
        action_len++;

    char action_buf[16];
    if (action_len >= sizeof(action_buf))
        return send_json_error(req, "404 Not Found", "unsupported operation");
    memcpy(action_buf, action, action_len);
    action_buf[action_len] = '\0';

    if (action_len == 0 || strcmp(action_buf, "config") == 0)
        return port_config_handler(req, tcp_port);
    if (strcmp(action_buf, "mode") == 0)
        return port_mode_handler(req, tcp_port);
    if (strcmp(action_buf, "disconnect") == 0)
        return port_disconnect_handler(req, tcp_port);

    return send_json_error(req, "404 Not Found", "unsupported operation");
}

bool web_server_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "Server already running");
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        s_server = NULL;
        return false;
    }

    httpd_uri_t uri_health = {
        .uri = "/api/health",
        .method = HTTP_GET,
        .handler = health_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_health);

    httpd_uri_t uri_ports_get = {
        .uri = "/api/ports",
        .method = HTTP_GET,
        .handler = ports_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ports_get);

    httpd_uri_t uri_ports_post = {
        .uri = "/api/ports",
        .method = HTTP_POST,
        .handler = ports_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_server, &uri_ports_post);

    httpd_uri_t uri_ports_action = {
        .uri = "/api/ports/*",
        .method = HTTP_POST,
        .handler = ports_action_handler,
        .user_ctx = NULL,
        .uri_match_fn = httpd_uri_match_wildcard
    };
    httpd_register_uri_handler(s_server, &uri_ports_action);

    return true;
}

void web_server_stop(void)
{
    if (!s_server)
        return;

    ESP_LOGI(TAG, "Stopping HTTP server");
    httpd_stop(s_server);
    s_server = NULL;
}
