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
 * Slices are decoded and pushed to the speaker as they arrive, so playback
 * starts early and memory stays flat no matter how long the sentence is.
 */
#include "sdkconfig.h"

#if CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/base64.h"

#include "voice_pipeline_private.h"

static const char *TAG = "volcengine_tts";

#define TTS_DEFAULT_URL         "https://openspeech.bytedance.com/api/v3/tts/unidirectional"
#define TTS_DEFAULT_RESOURCE_ID "seed-tts-2.0"
#define TTS_DEFAULT_SPEAKER     "zh_female_vv_uranus_bigtts"
#define TTS_HTTP_TIMEOUT_MS     20000
#define TTS_WRITE_TIMEOUT_MS    2000
/* Success codes. The API reference prints "code": 0 in its example, but the
 * live service sends 20000000 ("成功" in the shared Volcengine speech error
 * code table) alongside "message":"OK". Accept both. */
#define TTS_CODE_OK             0
#define TTS_CODE_SUCCESS        20000000
/* One JSON slice stays small; this only bounds a malformed reply. */
#define TTS_SLICE_MAX_BYTES (128 * 1024)

#define TTS_ERROR_SNIPPET_LEN 200

typedef struct {
    char *buf;        /* Holds the not-yet-complete trailing JSON object. */
    size_t len;
    size_t capacity;
    bool overflow;
    esp_err_t play_err;
    size_t total_pcm;
    /* First bytes of the raw body, kept NUL-terminated for error reporting.
     * buf cannot be used for this: complete objects are consumed out of it. */
    char error_snippet[TTS_ERROR_SNIPPET_LEN + 1];
    size_t error_snippet_len;
} tts_stream_t;

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

static esp_err_t tts_play_pcm(const uint8_t *pcm, size_t pcm_len)
{
    size_t offset = 0;

    while (offset < pcm_len) {
        size_t written = 0;
        esp_err_t err = voice_audio_io_write(pcm + offset, pcm_len - offset, &written,
                                             TTS_WRITE_TIMEOUT_MS);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Speaker write failed after %u bytes: %s", (unsigned)offset,
                     esp_err_to_name(err));
            return err;
        }
        if (written == 0) {
            ESP_LOGE(TAG, "Speaker stalled after %u bytes", (unsigned)offset);
            return ESP_ERR_TIMEOUT;
        }
        offset += written;
    }
    return ESP_OK;
}

/** Decode one `{"code":..,"data":"..."}` object and play its audio slice. */
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

    err = tts_play_pcm(pcm, pcm_len);
    stream->total_pcm += pcm_len;
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
        return tts_stream_append(stream, (const char *)evt->data, (size_t)evt->data_len);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Request construction                                                */
/* ------------------------------------------------------------------ */

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

    ESP_LOGI(TAG, "TTS request: resource=%s text='%.60s%s'", resource_id, text,
             strlen(text) > 60 ? "..." : "");

    json_body = tts_build_request_body(config, text);
    if (!json_body) {
        return ESP_ERR_NO_MEM;
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
        free(json_body);
        return ESP_FAIL;
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
        } else if (stream.play_err != ESP_OK) {
            err = stream.play_err;
        } else if (stream.total_pcm == 0) {
            ESP_LOGE(TAG, "TTS returned no audio");
            err = ESP_ERR_INVALID_RESPONSE;
        } else {
            ESP_LOGI(TAG, "Played %u bytes of PCM (~%u ms)", (unsigned)stream.total_pcm,
                     (unsigned)(stream.total_pcm * 1000 /
                                (size_t)(voice_audio_io_get_speaker_sample_rate() * 2)));
        }
    } else {
        ESP_LOGE(TAG, "TTS HTTP perform failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(stream.buf);
    free(api_key_header);
    free(json_body);
    return err;
}

#endif /* CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED */
