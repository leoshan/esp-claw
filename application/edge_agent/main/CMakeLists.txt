idf_component_register(
    SRC_DIRS "."
    INCLUDE_DIRS "."
    REQUIRES
        app_claw
        app_config
        cap_voice_pipeline
        cap_im_platform
        wifi_manager
        http_server
        captive_dns
        cmd_wifi
        esp_app_format
)
