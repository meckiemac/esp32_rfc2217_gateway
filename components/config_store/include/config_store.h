#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "adapters.h"

#ifdef __cplusplus
extern "C" {
#endif

bool config_store_load_ports(struct ser2net_esp32_serial_port_cfg *ports,
                             size_t max_ports,
                             size_t *out_count);

bool config_store_save_ports(const struct ser2net_esp32_serial_port_cfg *ports,
                             size_t count);

bool config_store_load_control(uint16_t *tcp_port, int *backlog);
bool config_store_save_control(uint16_t tcp_port, int backlog);

void config_store_clear_ports(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_STORE_H */
