idf_component_register(SRCS "avtp.c" "acmp.c" "aecp.c" "adp.c" "mrp.c" "maap.c" "msrp.c" "mvrp.c" "audio_output.c" "../audio/es8311.c"
        PRIV_REQUIRES esp_eth esp_netif esp_driver_i2s esp_driver_i2c esp_driver_gpio esp_codec_dev esp_timer audio
        INCLUDE_DIRS ".")

target_sources(${COMPONENT_LIB} PRIVATE "avtp.c")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
