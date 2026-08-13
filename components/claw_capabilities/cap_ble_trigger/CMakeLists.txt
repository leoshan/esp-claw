# NOTE: REQUIRES must stay unconditional. ESP-IDF evaluates component
# CMakeLists twice and CONFIG_* is undefined during the early dependency
# expansion pass, so gating requirements on Kconfig silently drops them.
# The capability is compiled out via #if CONFIG_CLAW_CAP_BLE_TRIGGER_ENABLED
# inside the sources instead.
idf_component_register(
    SRCS
        "src/cap_ble_trigger.c"
        "src/ble_trigger_stack.c"
        "src/ble_trigger_peripheral.c"
        "src/ble_trigger_hidh.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        claw_cap
        cap_voice_pipeline
        bt
        esp_hid
        nvs_flash
)
