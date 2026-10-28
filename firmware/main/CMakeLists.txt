idf_component_register(SRCS "app.cpp" "board.cpp" "render.cpp" "calendar.cpp" "network.cpp"
    INCLUDE_DIRS "."
    REQUIRES epaper esp_driver_i2c esp_driver_gpio esp_timer esp_wifi esp_netif esp_event esp_http_server esp_http_client nvs_flash json lwip mbedtls
    EMBED_FILES "web.html" "font20.bin" "font24.bin" "font32.bin" "font56.bin" "font112.bin" "font144.bin"
        "theme_a_clock.bin" "theme_a_large.bin" "theme_a_value.bin"
        "theme_b_clock.bin" "theme_b_large.bin" "theme_b_value.bin"
        "weather_serif.bin" "weather_sans.bin" "calendar_digits.bin")
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
