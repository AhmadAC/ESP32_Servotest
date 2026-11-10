# Locate dog-barking.wav across case differences on Linux CI runners
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../Sound/dog-barking.wav")
    set(AUDIO_FILE "${CMAKE_CURRENT_LIST_DIR}/../Sound/dog-barking.wav")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../sound/dog-barking.wav")
    set(AUDIO_FILE "${CMAKE_CURRENT_LIST_DIR}/../sound/dog-barking.wav")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/dog-barking.wav")
    set(AUDIO_FILE "${CMAKE_CURRENT_LIST_DIR}/dog-barking.wav")
else()
    file(GLOB_RECURSE SEARCH_WAV "${CMAKE_CURRENT_LIST_DIR}/../*dog-barking.wav")
    if(SEARCH_WAV)
        list(GET SEARCH_WAV 0 AUDIO_FILE)
    else()
        set(AUDIO_FILE "../Sound/dog-barking.wav")
    endif()
endif()

idf_component_register(
    SRCS "main.cpp" "audio_player.cpp" "ble_server.cpp" "keypad.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_http_server esp_wifi esp_netif nvs_flash driver esp_timer lwip bt
    EMBED_FILES "${AUDIO_FILE}"
)