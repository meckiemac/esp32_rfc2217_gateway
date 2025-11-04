#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ser2net_esp32_serial_port_cfg;

bool config_store_load_ports(struct ser2net_esp32_serial_port_cfg *ports,
                             size_t max_ports,
                             size_t *out_count);

bool config_store_save_ports(const struct ser2net_esp32_serial_port_cfg *ports,
                             size_t count);

bool config_store_load_control(uint16_t *tcp_port, int *backlog);
bool config_store_save_control(uint16_t tcp_port, int backlog);

void config_store_clear_ports(void);

bool config_store_load_wifi_credentials(char *ssid,
                                        size_t ssid_len,
                                        char *password,
                                        size_t password_len);
bool config_store_save_wifi_credentials(const char *ssid, const char *password);
void config_store_clear_wifi_credentials(void);

bool config_store_load_softap_forced_disable(bool *forced_disable);
bool config_store_save_softap_forced_disable(bool forced_disable);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_STORE_H */
