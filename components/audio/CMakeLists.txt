idf_component_register(SRCS "audio.c" "ak4619_tdm.c" "codec.c" "es8311.c"
        PRIV_REQUIRES driver esp_driver_i2s esp_driver_i2c esp_driver_gpio esp_codec_dev
        INCLUDE_DIRS ".")
