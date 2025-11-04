idf_build_get_property(idf_path IDF_PATH)
idf_build_get_property(project_dir PROJECT_DIR)
set(http_server_inc "${idf_path}/components/esp_http_server/include")
set(ser2net_inc "${project_dir}/lib/ser2net_mcu/include")
set(cjson_inc "${idf_path}/components/json")
idf_component_get_property(driver_dir esp_driver_uart COMPONENT_DIR)
set(driver_inc "${driver_dir}/include")
set(net_manager_inc "${project_dir}/components/net_manager/include")

idf_component_register(SRCS "web_server.c"
                      INCLUDE_DIRS "include" "${http_server_inc}" "${ser2net_inc}" "${cjson_inc}" "${driver_inc}" "${net_manager_inc}"
                      REQUIRES esp_http_server json esp_driver_uart
                      PRIV_REQUIRES esp_timer esp_system net_manager)
