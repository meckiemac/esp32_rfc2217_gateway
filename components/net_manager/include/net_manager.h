#ifndef NET_MANAGER_H
#define NET_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

struct net_manager_status {
    bool sta_configured;
    bool sta_connected;
    char sta_ssid[33];
    char sta_ip[16];
    bool ap_active;
    bool ap_force_disabled;
    uint32_t ap_remaining_seconds;
};

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

bool net_manager_apply_credentials(const char *ssid, const char *password);
void net_manager_forget_credentials(void);
bool net_manager_set_softap_forced_disable(bool forced_disable);
bool net_manager_get_status(struct net_manager_status *status);

#ifdef __cplusplus
}
#endif

#endif /* NET_MANAGER_H */
