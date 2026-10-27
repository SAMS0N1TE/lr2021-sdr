idf_component_register(SRCS "main.c" "radio.c" "board.c" "board_profile.c" "bridge.c" "wifi.c"
    "../../../src/lr2021_iq.c" "../../../src/lr2021_iq_dev.c" "../../../src/lr2021_iq_tx.c"
    "../../../src/lr2021_bridge.c" "../../../src/lr2021_bridge_server.c"
    INCLUDE_DIRS "." "../../../include"
    REQUIRES esp_driver_uart esp_driver_spi esp_driver_gpio esp_driver_i2c esp_timer console nvs_flash esp_wifi esp_netif esp_event lwip)
# The bridge output queue (bridge.c): 0.25 s at 250000 pairs/s.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LR2021_BRIDGE_SLOTS=128u)
