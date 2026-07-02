idf_component_register(
    SRCS
        "renderer.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        avatar_vm
    PRIV_REQUIRES
        esp_timer
        heap
        spiffs
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)

spiffs_create_partition_image(storage assets FLASH_IN_PROJECT)
