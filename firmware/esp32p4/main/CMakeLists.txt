idf_component_register(SRCS "main.c" "radio.c" "board.c" "board_profile.c" "bridge.c"
    "../../../src/lr2021_iq.c" "../../../src/lr2021_iq_dev.c" "../../../src/lr2021_iq_tx.c"
    "../../../src/lr2021_bridge.c" "../../../src/lr2021_bridge_server.c"
    INCLUDE_DIRS "." "../../../include"
    REQUIRES esp_driver_uart esp_driver_spi esp_driver_gpio esp_driver_i2c esp_timer console nvs_flash)
