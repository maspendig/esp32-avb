idf_component_register(SRCS "avtp.c" "acmp.c" "aecp.c" "adp.c"
        PRIV_REQUIRES esp_eth esp_netif
        INCLUDE_DIRS ".")

target_sources(${COMPONENT_LIB} PRIVATE "avtp.c")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
