#ifndef NET_MANAGER_H
#define NET_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise network stack (esp_netif, Wi-Fi, SNTP task).
 *
 * @return true on success, false otherwise.
 */
bool net_manager_init(void);

/**
 * @brief Start Wi-Fi connection process (STA mode with provisioned creds).
 */
void net_manager_start(void);

/**
 * @brief Stop Wi-Fi/SNTP and release resources.
 */
void net_manager_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_MANAGER_H */
