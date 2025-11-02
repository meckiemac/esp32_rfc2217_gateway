idf_component_get_property(nvs_dir nvs_flash COMPONENT_DIR)
set(nvs_inc "${nvs_dir}/include")
idf_component_get_property(ser2net_dir PROJECT_DIR)
set(ser2net_inc "${ser2net_dir}/lib/ser2net_mcu/include")

idf_component_register(SRCS "config_store.c"
                      INCLUDE_DIRS "include" "${nvs_inc}" "${ser2net_inc}"
                      REQUIRES nvs_flash)
