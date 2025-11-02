idf_component_get_property(event_dir esp_event COMPONENT_DIR)
idf_component_get_property(wifi_dir esp_wifi COMPONENT_DIR)
idf_component_get_property(netif_dir esp_netif COMPONENT_DIR)

set(event_inc "${event_dir}/include")
set(wifi_inc "${wifi_dir}/include")
set(netif_inc "${netif_dir}/include")

message(STATUS "net_manager event_inc=${event_inc}")

idf_component_register(SRCS "net_manager.c"
                      INCLUDE_DIRS "include" "${event_inc}" "${wifi_inc}" "${netif_inc}"
                      REQUIRES esp_event esp_wifi esp_netif lwip)
