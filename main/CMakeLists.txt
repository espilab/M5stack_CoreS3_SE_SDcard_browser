idf_component_register(
    SRCS "m5s3se_sd_browser.cpp"
    INCLUDE_DIRS ""
    REQUIRES
        tinyusb
        esp_http_server
        lwip
        esp_event
        nvs_flash
        m5unified
        fatfs
        vfs
        sdmmc
        esp_driver_sdspi
        esp_driver_spi
)
