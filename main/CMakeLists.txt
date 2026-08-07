idf_component_register(
    SRCS  "font_vietnamese_14.c" "display.c" "face_eyes.c" "wake_word_test.c" 
    INCLUDE_DIRS "." 
    REQUIRES 
        espressif__esp-sr 
        esp_driver_i2s 
        esp_wifi 
        nvs_flash 
        lwip 
        freertos 
        esp_ringbuf 
        espressif__esp_websocket_client
        esp_driver_gpio  
        lvgl
    
         PRIV_REQUIRES esp_mm esp_lcd driver freertos log
)