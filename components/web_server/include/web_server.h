#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise HTTP server instance (esp_http_server).
 *
 * Starts a minimal server exposing a health-check endpoint. Returns true on
 * success.
 */
bool web_server_start(void);

/**
 * @brief Stop the HTTP server if running.
 */
void web_server_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* WEB_SERVER_H */
