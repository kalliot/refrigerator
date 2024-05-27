idf_component_register(SRCS "app_main.c" "ds18b20.c" "cJSON.c" "temperatures.c"
                    "flashmem.c" "factoryreset.c" "cooler.c" "apwebserver/server.c" "ota/ota.c" INCLUDE_DIRS "."
                    EMBED_TXTFILES ${project_dir}/server_certs/ca_cert.pem)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
