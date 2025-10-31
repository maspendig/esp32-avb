idf_component_register(SRCS "avtp.c"
        PRIV_REQUIRES esp_eth esp_netif
        INCLUDE_DIRS "." "./include")

target_sources(${COMPONENT_LIB} PRIVATE "avtp.c")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
