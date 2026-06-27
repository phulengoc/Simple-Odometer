idf_component_register(
    SRCS "main.cpp"
         "status_screen.cpp"
         "tach_ring.cpp"
         "text8x8.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd_sh8601 read_lcd_id_bsp touch_bsp driver esp_timer)

# Quiet C++ warnings from the designated-initializer style used for IDF configs.
target_compile_options(__idf_main PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-missing-field-initializers>
)
