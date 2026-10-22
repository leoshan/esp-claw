# GPL-3.0: src/airplay is ported from UxPlay (via esp32-iphone-agent). Enabling this
# component makes the firmware binary GPL-3.0 (see src/airplay/LICENSE-GPL.md).
idf_component_register(
    SRCS
        "src/cap_phone_screen.c"
        "src/phone_stream.c"
        "src/airplay/rtsp.c"
        "src/airplay/handlers.c"
        "src/airplay/mirror.c"
        "src/airplay/pairing.c"
        "src/airplay/fairplay.c"
        "src/airplay/mdns.c"
        "src/airplay/playfair/playfair.c"
        "src/airplay/playfair/omg_hax.c"
        "src/airplay/playfair/hand_garble.c"
        "src/airplay/playfair/modified_md5.c"
        "src/airplay/playfair/sap_hash.c"
        "src/bplist/bplist.c"
        "src/video/gop.c"
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "src"
    REQUIRES
        claw_cap
        claw_core
    PRIV_REQUIRES
        esp_http_client
        esp_netif
        esp_timer
        lwip
        mbedtls
        nvs_flash
)
# upstream playfair/UxPlay code is not warning-clean under ESP-IDF's -Werror defaults
set_source_files_properties(
    src/airplay/playfair/omg_hax.c src/airplay/playfair/hand_garble.c
    src/airplay/playfair/modified_md5.c src/airplay/playfair/sap_hash.c src/airplay/playfair/playfair.c
    PROPERTIES COMPILE_OPTIONS "-Wno-error;-Wno-unused-variable;-Wno-unused-function")
