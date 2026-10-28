idf_component_register(SRCS "app.cpp" "board.cpp" "render.cpp" "network.cpp"
    INCLUDE_DIRS "."
    REQUIRES epaper esp_driver_i2c esp_driver_gpio esp_timer esp_wifi esp_netif esp_event esp_http_server esp_http_client nvs_flash json lwip mbedtls
    EMBED_FILES "web.html" "font20.bin" "font24.bin" "font32.bin" "font56.bin" "font112.bin" "font144.bin")
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
