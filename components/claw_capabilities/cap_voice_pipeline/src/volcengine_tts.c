/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Volcengine "单向流式语音合成" (unidirectional streaming TTS) over HTTP.
 *
 * POST /api/v3/tts/unidirectional answers with an HTTP chunked body carrying a
 * sequence of JSON objects, each holding one base64 slice of audio:
 *
 *     {"code":0,"message":"OK","data":"<base64 pcm>"}
 *
 * Slices are decoded into a PSRAM buffer while a separate task drains it to
 * the speaker, starting once TTS_PREFILL_SECONDS of audio have arrived.
 *
 * Playback must not run on the HTTP callback: i2s_channel_write blocks for as
 * long as the audio lasts, which throttles the TCP receive and starves the DMA
 * between slices, and every gap is an audible click. With playback decoupled,
 * measurements on hardware show the service delivering 114-122% of real time,
 * so a couple of seconds of prefill is enough to keep the buffer ahead without
 * waiting for the whole utterance.
 */
#include "sdkconfig.h"

#if CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "voice_pipeline_private.h"

static const char *TAG = "volcengine_tts";

#define TTS_DEFAULT_URL         "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
#define TTS_DEFAULT_RESOURCE_ID "seed-tts-2.0"
#define TTS_DEFAULT_SPEAKER     "zh_female_vv_uranus_bigtts"
#define TTS_HTTP_TIMEOUT_MS     20000
#define TTS_WRITE_TIMEOUT_MS    2000
/* ~170 ms of 24 kHz 16-bit mono, an order of magnitude under the timeout. */
#define TTS_WRITE_CHUNK_BYTES   8192
/* Success codes. The API reference prints "code": 0 in its example, but the
 * live service sends 20000000 ("成功" in the shared Volcengine speech error
 * code table) alongside "message":"OK". Accept both. */
#define TTS_CODE_OK             0
#define TTS_CODE_SUCCESS        20000000
/* One JSON slice stays small; this only bounds a malformed reply. */
#define TTS_SLICE_MAX_BYTES (128 * 1024)
/* Ceiling on buffered audio: 60 s at 24 kHz 16-bit mono is under 3 MB, which
 * fits PSRAM comfortably while bounding a runaway response. */
#define TTS_PCM_MAX_BYTES (24000 * 2 * 60)
/* Start playing once this many seconds are in hand. The producer stays ahead
 * of real time, so this is margin against jitter rather than a deficit. The
 * byte count comes from the board's actual rate, since a fixed one would mean
 * a different amount of audio on any board whose YAML disagrees. */
#define TTS_PREFILL_SECONDS 2
/* Frames and ESP_LOG only: the staging buffer is heap-allocated, because
 * xTaskCreate sizes the stack in bytes on ESP-IDF and TTS_WRITE_CHUNK_BYTES
 * alone would be twice this. */
#define TTS_PLAYER_TASK_STACK 4096
#define TTS_PLAYER_TASK_PRIO  6
#define TTS_PLAYER_IDLE_MS    20

#define TTS_ERROR_SNIPPET_LEN 200

typedef struct {
    char *buf;        /* Holds the not-yet-complete trailing JSON object. */
    size_t len;
    size_t capacity;
    bool overflow;
    esp_err_t play_err;
    size_t total_pcm;
    /* Decoded audio. Grows on the HTTP side, drains on the player side. */
    uint8_t *pcm;
    size_t pcm_len;
    size_t pcm_capacity;
    /* First bytes of the raw body, kept NUL-terminated for error reporting.
     * buf cannot be used for this: complete objects are consumed out of it. */
    char error_snippet[TTS_ERROR_SNIPPET_LEN + 1];
    size_t error_snippet_len;

    /* Playback side. pcm may be reallocated as it grows, so the player copies
     * out under the lock and never holds a pointer into it while writing. */
    SemaphoreHandle_t lock;
    SemaphoreHandle_t finished;
    TaskHandle_t player;
    bool producer_done;
    size_t played;
} tts_stream_t;

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

/** Prefer PSRAM for the audio buffer; a long reply is megabytes. */
static void *tts_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    return ptr ? ptr : malloc(size);
}

static esp_err_t tts_play_pcm(const uint8_t *pcm, size_t pcm_len)
{
    size_t offset = 0;

    /* Hand the driver one small chunk at a time. i2s_channel_write only
     * returns once the whole request has been queued, which takes as long as
     * that audio lasts, so passing the entire utterance would blow past any
     * sane timeout. TTS_WRITE_CHUNK_BYTES is a fraction of a second, leaving
     * the timeout to catch a genuinely stuck channel. */
    while (offset < pcm_len) {
        size_t remaining = pcm_len - offset;
        size_t chunk = remaining < TTS_WRITE_CHUNK_BYTES ? remaining : TTS_WRITE_CHUNK_BYTES;
        size_t written = 0;
        esp_err_t err = voice_audio_io_write(pcm + offset, chunk, &written,
                                             TTS_WRITE_TIMEOUT_MS);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Speaker write failed after %u/%u bytes: %s", (unsigned)offset,
                     (unsigned)pcm_len, esp_err_to_name(err));
            return err;
        }
        if (written == 0) {
            ESP_LOGE(TAG, "Speaker stalled after %u/%u bytes", (unsigned)offset,
                     (unsigned)pcm_len);
            return ESP_ERR_TIMEOUT;
        }
        offset += written;
    }
    return ESP_OK;
}

/** Append one decoded slice to the playback buffer, growing it in PSRAM. */
static esp_err_t tts_pcm_append(tts_stream_t *stream, const uint8_t *pcm, size_t len)
{
    esp_err_t err = ESP_OK;

    if (len == 0) {
        return ESP_OK;
    }

    xSemaphoreTake(stream->lock, portMAX_DELAY);

    if (stream->pcm_len + len > TTS_PCM_MAX_BYTES) {
        ESP_LOGE(TAG, "Synthesized audio exceeds %d bytes; truncating", TTS_PCM_MAX_BYTES);
        err = ESP_ERR_NO_MEM;
        goto out;
    }

    if (stream->pcm_len + len > stream->pcm_capacity) {
        size_t new_cap = stream->pcm_capacity ? stream->pcm_capacity * 2 : (128 * 1024);
        uint8_t *grown;
        uint8_t *old_buf;

        while (new_cap < stream->pcm_len + len) {
            new_cap *= 2;
        }
        if (new_cap > TTS_PCM_MAX_BYTES) {
            new_cap = TTS_PCM_MAX_BYTES;
        }

        /* Allocate and copy with the lock released. At the upper doublings
         * this moves megabytes through PSRAM, and holding the lock across it
         * would stall the player long enough to drain the I2S DMA, i.e. the
         * very click this design removes. Only this task appends, so pcm_len
         * cannot move underneath us, and the player keeps reading the old
         * buffer safely until the pointer is swapped. */
        xSemaphoreGive(stream->lock);
        grown = tts_alloc(new_cap);
        if (grown && stream->pcm_len > 0) {
            memcpy(grown, stream->pcm, stream->pcm_len);
        }
        xSemaphoreTake(stream->lock, portMAX_DELAY);

        if (!grown) {
            ESP_LOGE(TAG, "Failed to grow audio buffer to %u bytes", (unsigned)new_cap);
            err = ESP_ERR_NO_MEM;
            goto out;
        }

        old_buf = stream->pcm;
        stream->pcm = grown;
        stream->pcm_capacity = new_cap;
        /* Safe now: any player read in flight held the lock we just retook. */
        free(old_buf);
    }

    memcpy(stream->pcm + stream->pcm_len, pcm, len);
    stream->pcm_len += len;
    stream->total_pcm += len;

out:
    xSemaphoreGive(stream->lock);
    return err;
}

/**
 * Drain the buffer to the speaker as it fills.
 *
 * Copies each chunk out under the lock, because the producer may reallocate
 * the buffer to grow it and a pointer into the old block would dangle.
 */
static void tts_player_task(void *arg)
{
    tts_stream_t *stream = (tts_stream_t *)arg;
    uint8_t *stage = malloc(TTS_WRITE_CHUNK_BYTES);

    if (!stage) {
        xSemaphoreTake(stream->lock, portMAX_DELAY);
        stream->play_err = ESP_ERR_NO_MEM;
        xSemaphoreGive(stream->lock);
        xSemaphoreGive(stream->finished);
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        size_t chunk = 0;
        bool done;

        xSemaphoreTake(stream->lock, portMAX_DELAY);
        if (stream->pcm_len > stream->played) {
            chunk = stream->pcm_len - stream->played;
            if (chunk > TTS_WRITE_CHUNK_BYTES) {
                chunk = TTS_WRITE_CHUNK_BYTES;
            }
            memcpy(stage, stream->pcm + stream->played, chunk);
        }
        done = stream->producer_done;
        xSemaphoreGive(stream->lock);

        /* Attenuate the private copy, never stream->pcm: the buffer is the
         * record of what was synthesized, and scaling it in place would
         * compound on any replay. */
        voice_audio_io_apply_volume((int16_t *)stage, chunk / sizeof(int16_t));

        if (chunk == 0) {
            if (done) {
                break;
            }
            /* Underrun: the DMA drains during this wait, which is exactly what
             * the prefill exists to prevent. */
            vTaskDelay(pdMS_TO_TICKS(TTS_PLAYER_IDLE_MS));
            continue;
        }

        esp_err_t err = tts_play_pcm(stage, chunk);
        if (err != ESP_OK) {
            xSemaphoreTake(stream->lock, portMAX_DELAY);
            stream->play_err = err;
            xSemaphoreGive(stream->lock);
            break;
        }
        stream->played += chunk;
    }

    free(stage);
    xSemaphoreGive(stream->finished);
    vTaskDelete(NULL);
}

/** Start playback once enough audio is buffered to ride out jitter. */
static void tts_player_maybe_start(tts_stream_t *stream, bool force)
{
    if (stream->player) {
        return;
    }
    if (!force &&
        stream->pcm_len < (size_t)(voice_audio_io_get_speaker_sample_rate() * 2 *
                                   TTS_PREFILL_SECONDS)) {
        return;
    }
    if (stream->pcm_len == 0) {
        return;
    }

    ESP_LOGI(TAG, "Starting playback with %u bytes buffered (~%u ms)", (unsigned)stream->pcm_len,
             (unsigned)(stream->pcm_len * 1000 /
                        (size_t)(voice_audio_io_get_speaker_sample_rate() * 2)));

    if (xTaskCreate(tts_player_task, "tts_play", TTS_PLAYER_TASK_STACK, stream,
                    TTS_PLAYER_TASK_PRIO, &stream->player) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        stream->player = NULL;
        stream->play_err = ESP_ERR_NO_MEM;
    }
}

/** Decode one `{"code":..,"data":"..."}` object into the playback buffer. */
static esp_err_t tts_consume_object(tts_stream_t *stream, const char *json, size_t json_len)
{
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    cJSON *code;
    cJSON *data;
    const char *b64;
    size_t b64_len;
    size_t pcm_capacity;
    size_t pcm_len = 0;
    uint8_t *pcm;
    int mbed_ret;
    esp_err_t err;

    if (!root) {
        /* The caller only passes brace-balanced slices, so a parse failure
         * means a corrupt object rather than a truncated one. Report it
         * instead of silently leaving a hole in the audio. */
        ESP_LOGE(TAG, "Malformed TTS slice (%u bytes) dropped", (unsigned)json_len);
        return ESP_ERR_INVALID_RESPONSE;
    }

    code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code) && code->valueint != TTS_CODE_OK &&
        code->valueint != TTS_CODE_SUCCESS) {
        cJSON *message = cJSON_GetObjectItem(root, "message");

        ESP_LOGE(TAG, "TTS service returned code=%d message=%s", code->valueint,
                 (cJSON_IsString(message) && message->valuestring) ? message->valuestring : "(none)");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsString(data) || !data->valuestring || !data->valuestring[0]) {
        /* Trailing slices carry only timestamps/usage and no audio. */
        cJSON_Delete(root);
        return ESP_OK;
    }

    b64 = data->valuestring;
    b64_len = strlen(b64);
    /* base64 packs 3 bytes into 4 characters. */
    pcm_capacity = (b64_len / 4 + 1) * 3;
    pcm = malloc(pcm_capacity);
    if (!pcm) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    mbed_ret = mbedtls_base64_decode(pcm, pcm_capacity, &pcm_len, (const unsigned char *)b64,
                                     b64_len);
    cJSON_Delete(root);

    if (mbed_ret != 0) {
        ESP_LOGE(TAG, "base64 decode failed: -0x%04x", -mbed_ret);
        free(pcm);
        return ESP_ERR_INVALID_RESPONSE;
    }

    err = tts_pcm_append(stream, pcm, pcm_len);
    free(pcm);
    return err;
}

/* ------------------------------------------------------------------ */
/* Chunked JSON splitting                                              */
/* ------------------------------------------------------------------ */

/**
 * Find the end of the first complete top-level JSON object in @p buf.
 *
 * Chunk boundaries do not line up with object boundaries, so the stream is
 * scanned for balanced braces while ignoring braces inside strings.
 *
 * @return length of the object including its closing brace, or 0 if the buffer
 *         does not yet hold a complete one.
 */
static size_t tts_find_object_end(const char *buf, size_t len, size_t *out_start)
{
    size_t start = 0;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;

    while (start < len && buf[start] != '{') {
        start++;
    }
    if (start >= len) {
        return 0;
    }
    *out_start = start;

    for (size_t i = start; i < len; i++) {
        char c = buf[i];

        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\' && in_string) {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) {
            continue;
        }
        if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                return i - start + 1;
            }
        }
    }

    return 0;
}

static esp_err_t tts_stream_append(tts_stream_t *stream, const char *data, size_t len)
{
    size_t start = 0;
    size_t object_len;

    if (stream->error_snippet_len < TTS_ERROR_SNIPPET_LEN) {
        size_t room = TTS_ERROR_SNIPPET_LEN - stream->error_snippet_len;
        size_t copy = len < room ? len : room;

        memcpy(stream->error_snippet + stream->error_snippet_len, data, copy);
        stream->error_snippet_len += copy;
        stream->error_snippet[stream->error_snippet_len] = '\0';
    }

    if (stream->len + len > TTS_SLICE_MAX_BYTES) {
        if (!stream->overflow) {
            ESP_LOGE(TAG, "TTS slice exceeds %d bytes; aborting", TTS_SLICE_MAX_BYTES);
            stream->overflow = true;
        }
        return ESP_ERR_NO_MEM;
    }

    if (stream->len + len > stream->capacity) {
        size_t new_cap = stream->capacity ? stream->capacity * 2 : 8192;

        while (new_cap < stream->len + len) {
            new_cap *= 2;
        }
        char *grown = realloc(stream->buf, new_cap);
        if (!grown) {
            return ESP_ERR_NO_MEM;
        }
        stream->buf = grown;
        stream->capacity = new_cap;
    }

    memcpy(stream->buf + stream->len, data, len);
    stream->len += len;

    /* Drain every complete object currently buffered. */
    while ((object_len = tts_find_object_end(stream->buf, stream->len, &start)) > 0) {
        esp_err_t err = tts_consume_object(stream, stream->buf + start, object_len);

        if (err != ESP_OK) {
            stream->play_err = err;
            return err;
        }

        size_t consumed = start + object_len;
        memmove(stream->buf, stream->buf + consumed, stream->len - consumed);
        stream->len -= consumed;
        start = 0;
    }

    return ESP_OK;
}

static esp_err_t tts_http_event_handler(esp_http_client_event_t *evt)
{
    tts_stream_t *stream = (tts_stream_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && stream && evt->data_len > 0) {
        esp_err_t err = tts_stream_append(stream, (const char *)evt->data, (size_t)evt->data_len);

        /* Begin speaking as soon as there is a cushion, rather than waiting
         * for the whole utterance to synthesize. */
        tts_player_maybe_start(stream, false);
        return err;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Request construction                                                */
/* ------------------------------------------------------------------ */

/**
 * Rewrite markdown into something that reads naturally aloud.
 *
 * The service keeps markup verbatim by default, so "**你好**" is spoken as
 * "星星你好星星". Its `disable_markdown_filter` option would move this
 * server-side, but the API reference types `additions` as a string while
 * listing object fields under it, so the wire format is ambiguous. Stripping
 * here is unambiguous and also covers the model emitting markup despite being
 * asked not to.
 *
 * Markers are all ASCII, so a byte-wise pass is safe for UTF-8 text:
 * continuation bytes are always >= 0x80 and never collide with them.
 *
 * @return heap string the caller frees, or NULL on allocation failure.
 */
static char *tts_sanitize_text(const char *text)
{
    size_t len = strlen(text);
    /* Every rule either drops bytes or keeps them, so the input length is an
     * upper bound on the output. */
    char *out = malloc(len + 1);
    size_t o = 0;
    bool at_line_start = true;

    if (!out) {
        return NULL;
    }

    for (size_t i = 0; i < len;) {
        char c = text[i];

        if (at_line_start) {
            /* Drop heading hashes, block quotes and list bullets, each of
             * which the engine would otherwise pronounce. */
            if (c == ' ' || c == '\t') {
                i++;
                continue;
            }
            if (c == '#') {
                while (i < len && text[i] == '#') {
                    i++;
                }
                while (i < len && (text[i] == ' ' || text[i] == '\t')) {
                    i++;
                }
                continue;
            }
            if (c == '>' && i + 1 < len && (text[i + 1] == ' ' || text[i + 1] == '\t')) {
                i += 2;
                continue;
            }
            if ((c == '-' || c == '*' || c == '+') && i + 1 < len && text[i + 1] == ' ') {
                i += 2;
                continue;
            }
            at_line_start = false;
        }

        /* Emphasis and inline code markers. */
        if (c == '*' || c == '`') {
            i++;
            continue;
        }

        /* Strikethrough is a doubled tilde. A single one must survive: it is
         * the usual range separator, and dropping it turns "26~34℃" into
         * "2634℃". */
        if (c == '~' && i + 1 < len && text[i + 1] == '~') {
            i += 2;
            continue;
        }

        /* [label](url) -> label; a spoken URL is noise. */
        if (c == '[') {
            size_t close = i + 1;

            while (close < len && text[close] != ']' && text[close] != '\n') {
                close++;
            }
            if (close < len && text[close] == ']' && close + 1 < len && text[close + 1] == '(') {
                size_t paren = close + 2;

                while (paren < len && text[paren] != ')' && text[paren] != '\n') {
                    paren++;
                }
                if (paren < len && text[paren] == ')') {
                    for (size_t k = i + 1; k < close; k++) {
                        if (text[k] != '*' && text[k] != '`') {
                            out[o++] = text[k];
                        }
                    }
                    i = paren + 1;
                    continue;
                }
            }
        }

        if (c == '\n') {
            at_line_start = true;
        }
        out[o++] = c;
        i++;
    }

    out[o] = '\0';
    return out;
}

static char *tts_build_request_body(const app_config_t *config, const char *text)
{
    const char *speaker = config->tts_speaker[0] ? config->tts_speaker : TTS_DEFAULT_SPEAKER;
    cJSON *root = cJSON_CreateObject();
    cJSON *req_params;
    cJSON *audio_params;
    char *json;

    if (!root) {
        return NULL;
    }

    req_params = cJSON_AddObjectToObject(root, "req_params");
    cJSON_AddStringToObject(req_params, "text", text);
    cJSON_AddStringToObject(req_params, "speaker", speaker);

    audio_params = cJSON_AddObjectToObject(req_params, "audio_params");
    /* PCM is what the speaker takes directly, so no decoder is needed, and the
     * rate matches the board's I2S TX channel so no resampling is needed. */
    cJSON_AddStringToObject(audio_params, "format", "pcm");
    cJSON_AddNumberToObject(audio_params, "sample_rate", voice_audio_io_get_speaker_sample_rate());

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static void tts_build_uuid(char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%08" PRIx32 "-%04" PRIx32 "-4%03" PRIx32 "-%04" PRIx32 "-%08" PRIx32
                            "%04" PRIx32,
             esp_random(), esp_random() & 0xFFFF, esp_random() & 0xFFF,
             (esp_random() & 0x3FFF) | 0x8000, esp_random(), esp_random() & 0xFFFF);
}

esp_err_t volcengine_tts_speak(const app_config_t *config, const char *text)
{
    const char *url;
    const char *resource_id;
    char request_id[40];
    char *json_body = NULL;
    char *spoken = NULL;
    char *api_key_header = NULL;
    esp_http_client_handle_t client = NULL;
    tts_stream_t stream = {0};
    esp_err_t err;

    if (!config || !text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->voice_api_key[0]) {
        ESP_LOGE(TAG, "Voice API key is not configured");
        return ESP_ERR_INVALID_STATE;
    }

    url = config->tts_base_url[0] ? config->tts_base_url : TTS_DEFAULT_URL;
    resource_id = config->tts_resource_id[0] ? config->tts_resource_id : TTS_DEFAULT_RESOURCE_ID;
    tts_build_uuid(request_id, sizeof(request_id));

    spoken = tts_sanitize_text(text);
    if (!spoken) {
        return ESP_ERR_NO_MEM;
    }
    if (!spoken[0]) {
        ESP_LOGW(TAG, "Nothing left to speak after stripping markup");
        free(spoken);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "TTS request: resource=%s text='%.60s%s'", resource_id, spoken,
             strlen(spoken) > 60 ? "..." : "");

    json_body = tts_build_request_body(config, spoken);
    free(spoken);
    if (!json_body) {
        return ESP_ERR_NO_MEM;
    }

    stream.lock = xSemaphoreCreateMutex();
    stream.finished = xSemaphoreCreateBinary();
    if (!stream.lock || !stream.finished) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    esp_http_client_config_t http_cfg = {
        .url = url,
        .event_handler = tts_http_event_handler,
        .user_data = &stream,
        .timeout_ms = TTS_HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    client = esp_http_client_init(&http_cfg);
    if (!client) {
        err = ESP_FAIL;
        goto cleanup;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    /* New-console auth: a single X-Api-Key, no Bearer token. */
    esp_http_client_set_header(client, "X-Api-Key", config->voice_api_key);
    esp_http_client_set_header(client, "X-Api-Resource-Id", resource_id);
    esp_http_client_set_header(client, "X-Api-Request-Id", request_id);
    esp_http_client_set_post_field(client, json_body, strlen(json_body));

    err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);

        if (status != 200) {
            ESP_LOGE(TAG, "TTS HTTP status %d: %s", status,
                     stream.error_snippet_len ? stream.error_snippet : "(empty body)");
            err = ESP_FAIL;
        } else if (stream.pcm_len == 0) {
            ESP_LOGE(TAG, "TTS returned no audio");
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            /* A reply shorter than the prefill never tripped the threshold. */
            tts_player_maybe_start(&stream, true);
        }
    } else {
        ESP_LOGE(TAG, "TTS HTTP perform failed: %s", esp_err_to_name(err));
    }

    /* Tell the player no more audio is coming, then wait for it to drain. */
    if (stream.player) {
        xSemaphoreTake(stream.lock, portMAX_DELAY);
        stream.producer_done = true;
        xSemaphoreGive(stream.lock);

        /* Unbounded on purpose: every wait inside the player is bounded (the
         * per-chunk I2S timeout, and a mutex only ever held briefly), so it
         * always terminates. Giving up here instead would free the lock, the
         * buffer and this stack frame while the task still points at them. */
        xSemaphoreTake(stream.finished, portMAX_DELAY);
        ESP_LOGI(TAG, "Playback finished (%u of %u bytes)", (unsigned)stream.played,
                 (unsigned)stream.pcm_len);
    }

    /* Covers the task failing to start as well as failing mid-playback;
     * without this a failed start returns success having played nothing. */
    if (err == ESP_OK) {
        err = stream.play_err;
    }

cleanup:
    if (client) {
        esp_http_client_cleanup(client);
    }
    if (stream.lock) {
        vSemaphoreDelete(stream.lock);
    }
    if (stream.finished) {
        vSemaphoreDelete(stream.finished);
    }
    free(stream.buf);
    free(stream.pcm);
    free(api_key_header);
    free(json_body);
    return err;
}

#endif /* CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED */
