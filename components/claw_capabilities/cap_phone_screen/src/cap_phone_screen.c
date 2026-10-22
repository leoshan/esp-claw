/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Eyes of the phone agent: the board is an AirPlay receiver ("Screen Mirroring" on the phone),
 * forwards the H.264 stream to a relay on the LAN (phone_relay/relay.py) and fetches the latest
 * decoded frame from it as JPEG into the DATA root, where inspect_image and the agent read it.
 */
#include "cap_phone_screen.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_core_llm.h"
#include "claw_paths.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "phone_stream.h"
#include "airplay/airplay.h"

static const char *TAG = "cap_phone_screen";

#define PHONE_SCREEN_NVS_NS          "phone_screen"
#define PHONE_SCREEN_NVS_SEED        "ed25519_seed"
#define PHONE_SCREEN_DIR             "phone"
#define PHONE_SCREEN_FILE            "phone/screen.jpg"
#define PHONE_SCREEN_PATH_MAX        160
#define PHONE_SCREEN_URL_MAX         128
#define PHONE_SCREEN_HTTP_TIMEOUT_MS 5000
#define PHONE_SCREEN_IO_CHUNK        4096
#define PHONE_FRAME_MAX_BYTES        (512 * 1024)  /* a 540-wide JPEG is ~30-60 KB */
#define PHONE_SCREEN_READ_RETRIES    5     /* empty reads tolerated before a frame download is abandoned */
#define PHONE_LOOK_LOG_CHARS         600   /* model reply echoed to the console for each phone_look */
#define PHONE_SCREEN_WAIT_IP_MS      1000
#define PHONE_SCREEN_TASK_STACK      4096
#define PHONE_DIFF_SLOT_MAX          16    /* relay mark slot name, [A-Za-z0-9_] */

static int s_airplay_started;
static cap_phone_screen_core_getter_t s_get_core;

#define PHONE_LOOK_PROMPT_BASE \
    "You look at live screenshots of the user's iPhone, mirrored to an ESP32 board. " \
    "Follow the user prompt's reply format exactly. A grey round floating button is the AssistiveTouch " \
    "menu, a small grey translucent dot is the mouse pointer; neither is screen content. "

static const char *PHONE_LOOK_SYSTEM_PROMPT =
    PHONE_LOOK_PROMPT_BASE "Use the coordinate convention the user prompt asks for.";

static const char *PHONE_LOOK_GRID_SYSTEM_PROMPT =
    PHONE_LOOK_PROMPT_BASE
    "A magenta grid is drawn over the screenshot to help reading positions: thin lines divide it into 10 equal "
    "columns and 10 equal rows, thick lines mark the middle. The grid is not part of the phone screen. Use the "
    "coordinate convention the user prompt asks for.";

static void airplay_log(const char *msg)
{
    ESP_LOGI(TAG, "%s", msg);
}

/* Long-term AirPlay pairing identity: random once, then stable so the phone keeps trusting us. */
static void load_seed(uint8_t seed[32])
{
    nvs_handle_t nvs;
    size_t len = 32;

    if (nvs_open(PHONE_SCREEN_NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        esp_fill_random(seed, 32);
        return;
    }
    if (nvs_get_blob(nvs, PHONE_SCREEN_NVS_SEED, seed, &len) != ESP_OK || len != 32) {
        esp_fill_random(seed, 32);
        nvs_set_blob(nvs, PHONE_SCREEN_NVS_SEED, seed, 32);
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

static bool sta_has_ip(void)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;

    return sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr != 0;
}

static void airplay_start_task(void *arg)
{
    static airplay_cfg cfg;
    (void)arg;

    while (!sta_has_ip()) {
        vTaskDelay(pdMS_TO_TICKS(PHONE_SCREEN_WAIT_IP_MS));
    }
    phone_stream_start(CONFIG_CAP_PHONE_SCREEN_RELAY_HOST, CONFIG_CAP_PHONE_SCREEN_RELAY_STREAM_PORT);

    cfg.name = CONFIG_CAP_PHONE_SCREEN_RECEIVER_NAME;
    esp_read_mac(cfg.hwaddr, ESP_MAC_WIFI_STA);
    load_seed(cfg.ed25519_seed);
    cfg.width = CONFIG_CAP_PHONE_SCREEN_WIDTH;
    cfg.height = CONFIG_CAP_PHONE_SCREEN_HEIGHT;
    cfg.max_fps = CONFIG_CAP_PHONE_SCREEN_MAX_FPS;
    cfg.on_config = phone_stream_on_config;
    cfg.on_au = phone_stream_on_au;
    cfg.log = airplay_log;
    s_airplay_started = airplay_start(&cfg) ? 1 : -1;
    ESP_LOGI(TAG, "AirPlay receiver \"%s\" %s", cfg.name, s_airplay_started > 0 ? "started" : "failed to start");
    vTaskDelete(NULL);
}

/* GET http://relay/frame.jpg (grid: with the coordinate grid) into a PSRAM buffer the caller frees;
 * returns the HTTP status or -1. Kept in memory: writing the JPEG to the FAT partition and reading it
 * back cost ~1 s per look. */
static int fetch_frame_mem(bool grid, uint8_t **out_buf, size_t *out_bytes, char *err, size_t err_size)
{
    char url[PHONE_SCREEN_URL_MAX];
    uint8_t *buf = NULL;
    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = PHONE_SCREEN_HTTP_TIMEOUT_MS,
        .buffer_size = PHONE_SCREEN_IO_CHUNK,
    };
    esp_http_client_handle_t client;
    int status = -1;

    *out_buf = NULL;
    *out_bytes = 0;
    snprintf(url, sizeof(url), "http://%s:%d/frame.jpg%s", CONFIG_CAP_PHONE_SCREEN_RELAY_HOST,
             CONFIG_CAP_PHONE_SCREEN_RELAY_HTTP_PORT, grid ? "?grid=1" : "");
    client = esp_http_client_init(&http);
    if (!client) {
        snprintf(err, err_size, "http client init failed");
        return -1;
    }
    if (esp_http_client_open(client, 0) != ESP_OK) {
        snprintf(err, err_size, "cannot reach relay at %s", url);
        goto done;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    if (status != 200) {
        snprintf(err, err_size, "relay answered HTTP %d (no frame yet? start Screen Mirroring)", status);
        goto done;
    }
    if (content_length <= 0 || content_length > PHONE_FRAME_MAX_BYTES) {
        snprintf(err, err_size, "relay sent a frame of unexpected size (%lld bytes)", (long long)content_length);
        status = -1;
        goto done;
    }
    buf = heap_caps_malloc((size_t)content_length, MALLOC_CAP_SPIRAM);
    if (!buf) {
        snprintf(err, err_size, "out of memory for a %lld-byte frame", (long long)content_length);
        status = -1;
        goto done;
    }
    /* read exactly Content-Length: a short read under Wi-Fi load must not hand a truncated JPEG to
     * the vision model (it rejects it as "unsupported image") */
    for (int stalls = 0; (int64_t)*out_bytes < content_length;) {
        int n = esp_http_client_read(client, (char *)buf + *out_bytes, (int)(content_length - (int64_t)*out_bytes));
        if (n > 0) {
            *out_bytes += (size_t)n;
            stalls = 0;
            continue;
        }
        if (n < -ESP_ERR_HTTP_EAGAIN || ++stalls > PHONE_SCREEN_READ_RETRIES) {
            break;  /* a real error, or the relay stopped sending */
        }
    }
    if ((int64_t)*out_bytes != content_length) {
        snprintf(err, err_size, "frame truncated (%u of %lld bytes)", (unsigned)*out_bytes, (long long)content_length);
        status = -1;
    }
done:
    if (status == 200) {
        *out_buf = buf;
    } else {
        free(buf);
        *out_bytes = 0;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status;
}

/* fetch_frame_mem() into a file at path (phone_screenshot hands a path to inspect_image). */
static int fetch_frame(const char *path, bool grid, size_t *out_bytes, char *err, size_t err_size)
{
    uint8_t *buf = NULL;
    int status = fetch_frame_mem(grid, &buf, out_bytes, err, err_size);
    FILE *f;

    if (status != 200) {
        return status;
    }
    f = fopen(path, "wb");
    if (!f || fwrite(buf, 1, *out_bytes, f) != *out_bytes) {
        snprintf(err, err_size, f ? "write failed (storage full?)" : "cannot write %s", path);
        status = -1;
    }
    if (f) {
        fclose(f);
    }
    free(buf);
    return status;
}

/* GET http://relay<path> into out (small JSON answers such as /mark and /diff). */
static esp_err_t relay_get_text(const char *path, char *out, size_t out_size)
{
    char url[PHONE_SCREEN_URL_MAX];
    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = PHONE_SCREEN_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client;
    int len = 0;
    esp_err_t ret = ESP_FAIL;

    snprintf(url, sizeof(url), "http://%s:%d%s", CONFIG_CAP_PHONE_SCREEN_RELAY_HOST,
             CONFIG_CAP_PHONE_SCREEN_RELAY_HTTP_PORT, path);
    client = esp_http_client_init(&http);
    if (!client) {
        snprintf(out, out_size, "Error: http client init failed");
        return ESP_ERR_NO_MEM;
    }
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0 &&
            esp_http_client_get_status_code(client) == 200) {
        len = esp_http_client_read(client, out, out_size - 1);
        if (len >= 0) {
            out[len] = 0;
            ret = ESP_OK;
        }
    }
    if (ret != ESP_OK) {
        snprintf(out, out_size, "Error: relay request %s failed", path);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ret;
}

static esp_err_t phone_screen_diff_execute(const char *input_json, const claw_cap_call_context_t *ctx,
                                           char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    cJSON *op = root ? cJSON_GetObjectItem(root, "op") : NULL;
    cJSON *near = root ? cJSON_GetObjectItem(root, "near") : NULL;
    cJSON *slot = root ? cJSON_GetObjectItem(root, "slot") : NULL;
    bool pointer = root && cJSON_IsTrue(cJSON_GetObjectItem(root, "pointer"));
    char path[PHONE_SCREEN_URL_MAX] = {0};
    char slot_name[PHONE_DIFF_SLOT_MAX] = "default";
    esp_err_t ret;
    (void)ctx;

    if (cJSON_IsString(slot)) {
        size_t len = strlen(slot->valuestring);
        bool valid = len > 0 && len < sizeof(slot_name);
        for (size_t i = 0; valid && i < len; i++) {
            char c = slot->valuestring[i];
            valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }
        if (!valid) {
            cJSON_Delete(root);
            snprintf(output, output_size, "Error: slot must be 1-%d characters of [A-Za-z0-9_]",
                     PHONE_DIFF_SLOT_MAX - 1);
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(slot_name, slot->valuestring, sizeof(slot_name));
    }
    if (cJSON_IsString(op) && !strcmp(op->valuestring, "mark")) {
        snprintf(path, sizeof(path), "/mark?slot=%s", slot_name);
    } else if (cJSON_IsString(op) && !strcmp(op->valuestring, "still")) {
        snprintf(path, sizeof(path), "/still");
    } else if (cJSON_IsString(op) && !strcmp(op->valuestring, "diff")) {
        if (cJSON_IsArray(near) && cJSON_GetArraySize(near) == 2 &&
                cJSON_IsNumber(cJSON_GetArrayItem(near, 0)) && cJSON_IsNumber(cJSON_GetArrayItem(near, 1))) {
            snprintf(path, sizeof(path), "/diff?slot=%s&near=%d,%d%s", slot_name,
                     cJSON_GetArrayItem(near, 0)->valueint, cJSON_GetArrayItem(near, 1)->valueint,
                     pointer ? "&pointer=1" : "");
        } else {
            snprintf(path, sizeof(path), "/diff?slot=%s", slot_name);
        }
    }
    cJSON_Delete(root);
    if (!path[0]) {
        snprintf(output, output_size, "Error: op must be \"mark\", \"diff\" or \"still\"");
        return ESP_ERR_INVALID_ARG;
    }
    ret = relay_get_text(path, output, output_size);
    return ret;
}

static esp_err_t phone_screenshot_execute(const char *input_json, const claw_cap_call_context_t *ctx,
                                          char *output, size_t output_size)
{
    char dir[PHONE_SCREEN_PATH_MAX];
    char path[PHONE_SCREEN_PATH_MAX];
    char err[160] = {0};
    size_t bytes = 0;
    (void)input_json;
    (void)ctx;

    if (claw_paths_join(CLAW_PATH_DATA, PHONE_SCREEN_DIR, dir, sizeof(dir)) != ESP_OK ||
            claw_paths_join(CLAW_PATH_DATA, PHONE_SCREEN_FILE, path, sizeof(path)) != ESP_OK) {
        snprintf(output, output_size, "Error: DATA path is not ready");
        return ESP_ERR_INVALID_STATE;
    }
    mkdir(dir, 0775);
    if (fetch_frame(path, false, &bytes, err, sizeof(err)) != 200) {
        snprintf(output, output_size, "Error: %s", err);
        return ESP_FAIL;
    }
    snprintf(output, output_size, "{\"path\":\"%s\",\"bytes\":%u}", path, (unsigned)bytes);
    return ESP_OK;
}

/* Screenshot + vision model in one call, so Lua scripts (whose calls carry no core) can look too. */
static esp_err_t phone_look_execute(const char *input_json, const claw_cap_call_context_t *ctx,
                                    char *output, size_t output_size)
{
    claw_core_handle_t core = (ctx && ctx->core) ? ctx->core : (s_get_core ? s_get_core() : NULL);
    claw_media_asset_t asset = {0};
    claw_llm_media_request_t request = {0};
    char err[160] = {0};
    char *text = NULL;
    char *llm_err = NULL;
    uint8_t *frame = NULL;
    size_t bytes = 0;
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    cJSON *prompt = root ? cJSON_GetObjectItem(root, "prompt") : NULL;
    bool grid = root && cJSON_IsTrue(cJSON_GetObjectItem(root, "grid"));
    esp_err_t ret;

    if (!cJSON_IsString(prompt) || !prompt->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: prompt is required");
        return ESP_ERR_INVALID_ARG;
    }
    if (!core) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: claw_core is not ready");
        return ESP_ERR_INVALID_STATE;
    }
    int64_t started_us = esp_timer_get_time();
    if (fetch_frame_mem(grid, &frame, &bytes, err, sizeof(err)) != 200) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: %s", err);
        return ESP_FAIL;
    }
    asset.kind = CLAW_MEDIA_ASSET_KIND_INLINE_BYTES;
    asset.bytes = frame;
    asset.byte_count = bytes;
    asset.mime_type = "image/jpeg";
    request.system_prompt = grid ? PHONE_LOOK_GRID_SYSTEM_PROMPT : PHONE_LOOK_SYSTEM_PROMPT;
    request.user_prompt = prompt->valuestring;
    request.media = &asset;
    request.media_count = 1;
    int64_t fetched_us = esp_timer_get_time();
    ret = claw_core_llm_infer_media(core, &request, &text, &llm_err);
    int64_t inferred_us = esp_timer_get_time();
    cJSON_Delete(root);
    if (ret != ESP_OK) {
        snprintf(output, output_size, "Error: vision model failed (%s)%s%s", esp_err_to_name(ret),
                 llm_err ? ": " : "", llm_err ? llm_err : "");
    } else {
        snprintf(output, output_size, "%s", text ? text : "");
        ESP_LOGI(TAG, "phone_look (%u-byte frame, fetch %d ms, model %d ms) -> %.*s", (unsigned)bytes,
                 (int)((fetched_us - started_us) / 1000), (int)((inferred_us - fetched_us) / 1000),
                 PHONE_LOOK_LOG_CHARS, text ? text : "");
    }
    free(text);
    free(llm_err);
    free(frame);
    return ret;
}

static esp_err_t phone_screen_status_execute(const char *input_json, const claw_cap_call_context_t *ctx,
                                             char *output, size_t output_size)
{
    static const char *states[] = {"idle", "connected", "streaming"};
    phone_stream_stats_t st;
    int64_t age = -1;
    int state = airplay_state();
    (void)input_json;
    (void)ctx;

    phone_stream_get_stats(&st);
    if (st.last_au_ms) {
        age = esp_timer_get_time() / 1000 - st.last_au_ms;
    }
    snprintf(output, output_size,
             "{\"receiver\":\"%s\",\"started\":%s,\"airplay\":\"%s\",\"relay\":\"%s:%d\","
             "\"relay_connected\":%s,\"aus\":%u,\"idrs\":%u,\"dropped\":%u,\"sent_bytes\":%u,\"last_au_age_ms\":%lld}",
             CONFIG_CAP_PHONE_SCREEN_RECEIVER_NAME, s_airplay_started > 0 ? "true" : "false",
             state >= 0 && state <= 2 ? states[state] : "?", CONFIG_CAP_PHONE_SCREEN_RELAY_HOST,
             CONFIG_CAP_PHONE_SCREEN_RELAY_STREAM_PORT, st.relay_connected ? "true" : "false",
             (unsigned)st.au_count, (unsigned)st.idr_count, (unsigned)st.dropped, (unsigned)st.bytes_sent,
             (long long)age);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_phone_screen_descriptors[] = {
    {
        .id = "phone_screenshot",
        .name = "phone_screenshot",
        .family = "phone",
        .description =
        "Capture the current screen of the phone that is mirroring to this board (AirPlay) and save it as a "
        "JPEG in the DATA root. Returns {path, bytes}; pass the path to inspect_image to look at it.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = phone_screenshot_execute,
    },
    {
        .id = "phone_look",
        .name = "phone_look",
        .family = "phone",
        .description =
        "Take a fresh screenshot of the mirrored phone and ask the vision model about it in one step. "
        "Input {prompt, grid}: what to look for and the reply format; grid=true overlays a 10x10 coordinate "
        "grid (helps locating, but hides small text). Returns the model's answer.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"},\"grid\":{\"type\":\"boolean\"}},\"required\":[\"prompt\"]}",
        .execute = phone_look_execute,
    },
    {
        .id = "phone_screen_diff",
        .name = "phone_screen_diff",
        .family = "phone",
        .description =
        "Pixel change detector on the mirrored phone screen. op=still returns {since_ms} (how long the screen "
        "has not changed); op=mark remembers the current frame; op=diff "
        "returns {changed, bbox, center} (0-1000 coordinates) of what changed since mark; optional near=[x,y] "
        "only looks around that point; with near, pointer=true means the pointer was nudged by +x +y since mark: "
        "the region that looks like a moved pointer is picked and its resting position returned as rest=[x,y]; "
        "optional slot=name keeps a separate mark per name. Used to find the pointer and to check whether an "
        "action changed the screen.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"op\":{\"type\":\"string\",\"enum\":[\"mark\",\"diff\",\"still\"]},\"near\":{\"type\":\"array\",\"items\":{\"type\":\"number\"}},\"pointer\":{\"type\":\"boolean\"},\"slot\":{\"type\":\"string\"}},\"required\":[\"op\"]}",
        .execute = phone_screen_diff_execute,
    },
    {
        .id = "phone_screen_status",
        .name = "phone_screen_status",
        .family = "phone",
        .description =
        "Status of the phone screen link: AirPlay receiver state, relay connection and stream counters.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = phone_screen_status_execute,
    },
};

static const claw_cap_group_t s_phone_screen_group = {
    .group_id = "cap_phone_screen",
    .descriptors = s_phone_screen_descriptors,
    .descriptor_count = sizeof(s_phone_screen_descriptors) / sizeof(s_phone_screen_descriptors[0]),
};

esp_err_t cap_phone_screen_register_group(cap_phone_screen_core_getter_t get_core)
{
    s_get_core = get_core;
    if (claw_cap_group_exists(s_phone_screen_group.group_id)) {
        return ESP_OK;
    }
    if (xTaskCreate(airplay_start_task, "phone_airplay", PHONE_SCREEN_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return claw_cap_register_group(&s_phone_screen_group);
}
