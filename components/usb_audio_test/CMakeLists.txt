if(IDF_TARGET STREQUAL "esp32s3")
    idf_component_register(
        SRCS "usb_audio_test.c"
        INCLUDE_DIRS "include"
        PRIV_REQUIRES usb espressif__usb_host_uac console platform_config
    )
else()
    idf_component_register(
        INCLUDE_DIRS "include"
    )
endif()
