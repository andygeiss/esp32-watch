# The host layer, device edition. esp_lcd_sh8601 and esp_lcd_touch_ft5x06 are
# managed dependencies — see idf_component.yml — and the component manager adds
# them to the requirements itself.
idf_component_register(
    SRCS "main.c" "board.c" "net.c"
    INCLUDE_DIRS "."
    REQUIRES kai_ui lvgl driver esp_lcd esp_timer esp_wifi esp_netif nvs_flash)
