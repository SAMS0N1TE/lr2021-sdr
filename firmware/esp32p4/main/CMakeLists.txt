idf_component_register(SRCS "main.c" "radio.c" "../../../src/lr2021_iq.c"
    INCLUDE_DIRS "." "../../../include"
    REQUIRES esp_driver_spi esp_driver_gpio esp_driver_i2c esp_timer console)
