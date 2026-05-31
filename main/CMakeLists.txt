idf_component_register(
    SRCS "example_qspi_with_ram.cpp"
          "ble_pairing.cpp"
          "status_screen.cpp"
          "nav_hud.cpp"
    INCLUDE_DIRS "." "../components/jpegdec/test_images"
    REQUIRES esp_wifi esp_netif lwip nvs_flash esp_timer esp_lcd_sh8601 touch_bsp read_lcd_id_bsp lvgl driver spi_flash jpegdec bt)

# Add C++ compiler flags to handle designated initializer warnings
target_compile_options(__idf_main PRIVATE 
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-designated-init>
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-c99-designator>
)



set_source_files_properties(
    ${LV_DEMOS_SOURCES}
    PROPERTIES COMPILE_OPTIONS
    -DLV_LVGL_H_INCLUDE_SIMPLE
    )
