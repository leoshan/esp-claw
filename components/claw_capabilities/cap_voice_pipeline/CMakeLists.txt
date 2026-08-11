# NOTE: REQUIRES must stay unconditional. ESP-IDF evaluates component
# CMakeLists twice and CONFIG_* is undefined during the early dependency
# expansion pass, so gating requirements on Kconfig silently drops them.
# The capability is compiled out via #if CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED
# inside the sources instead.
idf_component_register(
    SRCS
        "src/cap_voice_pipeline.c"
        "src/volcengine_sensevoice_asr.c"
        "src/volcengine_tts.c"
        "src/i2s_audio_driver.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        claw_cap
        claw_event_router
        cap_im_local
        app_config
        esp_http_client
        driver
        esp_driver_gpio
        esp_driver_i2s
        esp_hw_support
        mbedtls
)
