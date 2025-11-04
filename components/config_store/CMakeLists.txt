idf_build_get_property(project_dir PROJECT_DIR)

idf_component_register(SRCS "config_store.c"
                      INCLUDE_DIRS "include" "${project_dir}/lib/ser2net_mcu/include"
                      REQUIRES nvs_flash
                      PRIV_REQUIRES driver esp_driver_uart)
