idf_component_register(
    SRCS "main.cpp" "audio_player.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi esp_netif nvs_flash driver esp_timer lwip esp_driver_i2s
    EMBED_FILES "../Sound/dog-barking.wav"
)