idf_component_register(SRCS "srpd.c" "msrp.c"
        PRIV_REQUIRES esp_eth esp_netif
        INCLUDE_DIRS ".")

target_sources(${COMPONENT_LIB} PRIVATE "srpd.c")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
