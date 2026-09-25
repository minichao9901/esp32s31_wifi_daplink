idf_component_register(SRCS "wifi_dap_main.c"
                            "daplink/DAP.c"
                            "daplink/SW_DP.c"
                            "daplink/SW_DP_fast.c"
                            "daplink/SW_DP_slow.c"
                            "daplink/dedic_swd.S"
                            "daplink/dedic_selftest.c"
                       INCLUDE_DIRS "." "daplink"
                       REQUIRES esp_driver_gpio
                       PRIV_REQUIRES esp_wifi esp_netif nvs_flash lwip esp_timer)
