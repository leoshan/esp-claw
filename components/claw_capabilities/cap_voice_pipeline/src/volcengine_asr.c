/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Volcengine "大模型流式语音识别" (streaming ASR) client.
 *
 * Uses the 流式输入模式 endpoint (.../sauc/bigmodel_nostream): PCM is streamed
 * up as it is captured and the service answers with the final transcript after
 * the last (negative-sequence) packet. That mode is more accurate than the
 * bidirectional one and, since the agent only consumes a finished sentence,
 * partial results would be thrown away anyway.
 *
 * Wire format (all integers big-endian):
 *
 *   byte 0: protocol version (4b) | header size in 4-byte words (4b)
 *   byte 1: message type (4b)     | message type specific flags (4b)
 *   byte 2: serialization (4b)    | compression (4b)
 *   byte 3: reserved
 *   [4-byte sequence, when the flags say so]
 *   4-byte payload size
 *   payload
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
#include "esp_log.h"
#include "esp_random.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "voice_pipeline_private.h"

static const char *TAG = "volcengine_asr";

#define ASR_DEFAULT_URL         "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream"
#define ASR_DEFAULT_RESOURCE_ID "volc.seedasr.sauc.duration"
#define ASR_MODEL_NAME          "bigmodel"

/* Protocol constants */
#define ASR_PROTOCOL_VERSION  0x01
#define ASR_HEADER_WORDS      0x01 /* header is 1 x 4 = 4 bytes */
#define ASR_MSG_FULL_CLIENT   0x01
#define ASR_MSG_AUDIO_ONLY    0x02
#define ASR_MSG_FULL_SERVER   0x09
#define ASR_MSG_ERROR         0x0F
#define ASR_FLAG_NONE         0x00
#define ASR_FLAG_POS_SEQUENCE 0x01
#define ASR_FLAG_NEG_SEQUENCE 0x03
#define ASR_SERIAL_NONE       0x00
#define ASR_SERIAL_JSON       0x01
#define ASR_COMPRESS_NONE     0x00

#define ASR_HEADER_SIZE 4
/* 200 ms of 16 kHz 16-bit mono audio, the packet size the docs recommend. */
#define ASR_PACKET_MS       200
#define ASR_PACKET_BYTES    (VOICE_ASR_SAMPLE_RATE * 2 * ASR_PACKET_MS / 1000)
#define ASR_READ_TIMEOUT_MS 300
#define ASR_SEND_TIMEOUT_MS 5000
/* Give up rather than spin if the microphone never yields samples. */
#define ASR_MAX_EMPTY_READS 20
/* Longest utterance we will capture before forcing the final packet. */
#define ASR_MAX_CAPTURE_MS   (CONFIG_CLAW_CAP_VOICE_RECORD_DURATION_SEC * 1000)
#define ASR_CONNECT_WAIT_MS  10000
#define ASR_RESULT_WAIT_MS   15000
#define ASR_WS_BUFFER_SIZE   8192
#define ASR_WS_TASK_STACK    6144

#define ASR_EVT_CONNECTED (1 << 0)
#define ASR_EVT_RESULT    (1 << 1)
#define ASR_EVT_FAILED    (1 << 2)

typedef struct {
    EventGroupHandle_t events;
    char *text;      /* Latest transcript seen, heap-allocated. */
    char *rx_buf;    /* Reassembly buffer for fragmented server frames. */
    size_t rx_len;
    size_t rx_capacity;
    bool final_sent; /* Set once the negative-sequence packet went out. */
} asr_session_t;

/* ------------------------------------------------------------------ */
/* Framing helpers                                                     */
/* ------------------------------------------------------------------ */

static void asr_write_u32_be(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

static uint32_t asr_read_u32_be(const uint8_t *src)
{
    return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) |
           (uint32_t)src[3];
}

static void asr_fill_header(uint8_t *header, uint8_t msg_type, uint8_t flags, uint8_t serialization)
{
    header[0] = (uint8_t)((ASR_PROTOCOL_VERSION << 4) | ASR_HEADER_WORDS);
    header[1] = (uint8_t)((msg_type << 4) | flags);
    header[2] = (uint8_t)((serialization << 4) | ASR_COMPRESS_NONE);
    header[3] = 0;
}

/**
 * Build one protocol frame: header + optional sequence + payload size + payload.
 * Returns a heap buffer the caller must free, and its length in @p out_len.
 */
static uint8_t *asr_build_frame(uint8_t msg_type, uint8_t serialization, int32_t sequence,
                                bool with_sequence, const void *payload, size_t payload_len,
                                size_t *out_len)
{
    size_t seq_len = with_sequence ? 4 : 0;
    size_t total = ASR_HEADER_SIZE + seq_len + 4 + payload_len;
    uint8_t *frame = malloc(total);
    uint8_t flags;
    size_t offset;

    if (!frame) {
        return NULL;
    }

    if (!with_sequence) {
        flags = ASR_FLAG_NONE;
    } else {
        flags = (sequence < 0) ? ASR_FLAG_NEG_SEQUENCE : ASR_FLAG_POS_SEQUENCE;
    }

    asr_fill_header(frame, msg_type, flags, serialization);
    offset = ASR_HEADER_SIZE;

    if (with_sequence) {
        asr_write_u32_be(frame + offset, (uint32_t)sequence);
        offset += 4;
    }

    asr_write_u32_be(frame + offset, (uint32_t)payload_len);
    offset += 4;

    if (payload_len > 0) {
        memcpy(frame + offset, payload, payload_len);
    }

    *out_len = total;
    return frame;
}

/* ------------------------------------------------------------------ */
/* Server frame parsing                                                */
/* ------------------------------------------------------------------ */

static void asr_store_text(asr_session_t *session, const char *text)
{
    char *copy = strdup(text);

    if (!copy) {
        return;
    }
    free(session->text);
    session->text = copy;
}

/** Extract `result.text` from a full server response payload. */
static void asr_parse_payload(asr_session_t *session, const char *json, size_t json_len)
{
    cJSON *root = cJSON_ParseWithLength(json, json_len);
    cJSON *result;
    cJSON *text;

    if (!root) {
        ESP_LOGW(TAG, "Unparseable server payload");
        return;
    }

    result = cJSON_GetObjectItem(root, "result");
    /* The docs describe result as a list but the service sends an object;
     * accept either so a format change does not silently drop the text. */
    if (cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
        result = cJSON_GetArrayItem(result, 0);
    }

    text = result ? cJSON_GetObjectItem(result, "text") : NULL;
    if (cJSON_IsString(text) && text->valuestring && text->valuestring[0]) {
        asr_store_text(session, text->valuestring);
        ESP_LOGD(TAG, "Transcript so far: %s", text->valuestring);
    }

    cJSON_Delete(root);
}

static void asr_handle_server_frame(asr_session_t *session, const uint8_t *data, size_t len)
{
    uint8_t msg_type;
    uint8_t flags;
    size_t offset;
    uint32_t payload_len;

    if (len < ASR_HEADER_SIZE + 4) {
        return;
    }

    msg_type = (uint8_t)(data[1] >> 4);
    flags = (uint8_t)(data[1] & 0x0F);
    offset = (size_t)(data[0] & 0x0F) * 4;
    if (offset < ASR_HEADER_SIZE) {
        offset = ASR_HEADER_SIZE;
    }

    /* Skip the sequence field when the flags announce one. */
    if (flags == ASR_FLAG_POS_SEQUENCE || flags == ASR_FLAG_NEG_SEQUENCE) {
        if (len < offset + 4) {
            return;
        }
        offset += 4;
    }

    if (len < offset + 4) {
        return;
    }
    payload_len = asr_read_u32_be(data + offset);
    offset += 4;

    if (payload_len > len - offset) {
        payload_len = (uint32_t)(len - offset);
    }

    if (msg_type == ASR_MSG_ERROR) {
        /* Error frames are header | error_code(4) | message_size(4) | message,
         * so what was read as payload_len above is really the error code. */
        uint32_t error_code = payload_len;
        uint32_t message_len = 0;

        if (len >= offset + 4) {
            message_len = asr_read_u32_be(data + offset);
            offset += 4;
            if (message_len > len - offset) {
                message_len = (uint32_t)(len - offset);
            }
        }

        ESP_LOGE(TAG, "ASR service error %" PRIu32 ": %.*s", error_code, (int)message_len,
                 (const char *)data + offset);
        xEventGroupSetBits(session->events, ASR_EVT_FAILED);
        return;
    }

    if (msg_type != ASR_MSG_FULL_SERVER || payload_len == 0) {
        return;
    }

    asr_parse_payload(session, (const char *)data + offset, payload_len);

    /* In 流式输入模式 the transcript that follows our final packet is the
     * complete one, so that is when the caller can stop waiting. */
    if (session->final_sent) {
        xEventGroupSetBits(session->events, ASR_EVT_RESULT);
    }
}

/** Reassemble frames that the client delivered across multiple events. */
static void asr_accumulate(asr_session_t *session, const char *data, int data_len, int payload_len,
                           int payload_offset)
{
    if (payload_offset == 0) {
        session->rx_len = 0;
    }

    if (session->rx_len + (size_t)data_len > session->rx_capacity) {
        size_t needed = session->rx_len + (size_t)data_len;
        char *grown = realloc(session->rx_buf, needed);

        if (!grown) {
            ESP_LOGE(TAG, "Out of memory reassembling %u-byte frame", (unsigned)needed);
            session->rx_len = 0;
            return;
        }
        session->rx_buf = grown;
        session->rx_capacity = needed;
    }

    memcpy(session->rx_buf + session->rx_len, data, (size_t)data_len);
    session->rx_len += (size_t)data_len;

    if ((int)session->rx_len >= payload_len) {
        asr_handle_server_frame(session, (const uint8_t *)session->rx_buf, session->rx_len);
        session->rx_len = 0;
    }
}

static void asr_websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                                        void *event_data)
{
    asr_session_t *session = (asr_session_t *)handler_args;
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    (void)base;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ASR WebSocket connected");
        xEventGroupSetBits(session->events, ASR_EVT_CONNECTED);
        break;
    case WEBSOCKET_EVENT_DATA:
        /* op_code 0x2 is binary; ignore ping/pong/close frames. */
        if (data->op_code == 0x02 && data->data_len > 0) {
            asr_accumulate(session, data->data_ptr, data->data_len, data->payload_len,
                           data->payload_offset);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "ASR WebSocket error");
        xEventGroupSetBits(session->events, ASR_EVT_FAILED);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "ASR WebSocket disconnected");
        xEventGroupSetBits(session->events, ASR_EVT_FAILED);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Request construction                                                */
/* ------------------------------------------------------------------ */

static char *asr_build_config_payload(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *user;
    cJSON *audio;
    cJSON *request;
    char *json;

    if (!root) {
        return NULL;
    }

    user = cJSON_AddObjectToObject(root, "user");
    cJSON_AddStringToObject(user, "uid", "esp_claw_device");

    audio = cJSON_AddObjectToObject(root, "audio");
    cJSON_AddStringToObject(audio, "format", "pcm");
    cJSON_AddStringToObject(audio, "codec", "raw");
    cJSON_AddNumberToObject(audio, "rate", VOICE_ASR_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio, "bits", VOICE_ASR_BITS);
    cJSON_AddNumberToObject(audio, "channel", VOICE_ASR_CHANNELS);

    request = cJSON_AddObjectToObject(root, "request");
    cJSON_AddStringToObject(request, "model_name", ASR_MODEL_NAME);
    cJSON_AddBoolToObject(request, "enable_itn", true);
    cJSON_AddBoolToObject(request, "enable_punc", true);

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static void asr_build_uuid(char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%08" PRIx32 "-%04" PRIx32 "-4%03" PRIx32 "-%04" PRIx32 "-%08" PRIx32
                            "%04" PRIx32,
             esp_random(), esp_random() & 0xFFFF, esp_random() & 0xFFF,
             (esp_random() & 0x3FFF) | 0x8000, esp_random(), esp_random() & 0xFFFF);
}

static char *asr_build_headers(const app_config_t *config, const char *request_id)
{
    const char *resource_id =
        config->asr_resource_id[0] ? config->asr_resource_id : ASR_DEFAULT_RESOURCE_ID;
    size_t len = strlen(config->voice_api_key) + strlen(resource_id) + strlen(request_id) + 128;
    char *headers = malloc(len);

    if (!headers) {
        return NULL;
    }

    /* New-console auth: a single X-Api-Key, no app id / access token pair. */
    snprintf(headers, len,
             "X-Api-Key: %s\r\n"
             "X-Api-Resource-Id: %s\r\n"
             "X-Api-Request-Id: %s\r\n"
             "X-Api-Sequence: -1\r\n",
             config->voice_api_key, resource_id, request_id);
    return headers;
}

static esp_err_t asr_send_frame(esp_websocket_client_handle_t client, uint8_t msg_type,
                                uint8_t serialization, int32_t sequence, bool with_sequence,
                                const void *payload, size_t payload_len)
{
    size_t frame_len = 0;
    uint8_t *frame = asr_build_frame(msg_type, serialization, sequence, with_sequence, payload,
                                     payload_len, &frame_len);
    int sent;

    if (!frame) {
        return ESP_ERR_NO_MEM;
    }

    sent = esp_websocket_client_send_bin(client, (const char *)frame, (int)frame_len,
                                         pdMS_TO_TICKS(ASR_SEND_TIMEOUT_MS));
    free(frame);

    if (sent < 0) {
        ESP_LOGE(TAG, "WebSocket send failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Public entry point                                                  */
/* ------------------------------------------------------------------ */

esp_err_t volcengine_asr_stream_recognize(const app_config_t *config,
                                          bool (*should_continue)(void), char *out_text,
                                          size_t max_text_len)
{
    const char *url;
    char request_id[40];
    char *headers = NULL;
    char *config_payload = NULL;
    uint8_t *pcm_chunk = NULL;
    esp_websocket_client_handle_t client = NULL;
    asr_session_t session = {0};
    EventBits_t bits;
    esp_err_t err = ESP_FAIL;
    int32_t sequence = 1;
    int elapsed_ms = 0;
    int empty_reads = 0;

    if (!config || !out_text || max_text_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out_text[0] = '\0';

    if (!config->voice_api_key[0]) {
        ESP_LOGE(TAG, "Voice API key is not configured");
        return ESP_ERR_INVALID_STATE;
    }

    url = config->asr_base_url[0] ? config->asr_base_url : ASR_DEFAULT_URL;
    asr_build_uuid(request_id, sizeof(request_id));

    session.events = xEventGroupCreate();
    pcm_chunk = malloc(ASR_PACKET_BYTES);
    headers = asr_build_headers(config, request_id);
    config_payload = asr_build_config_payload();

    if (!session.events || !pcm_chunk || !headers || !config_payload) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = url,
        .headers = headers,
        .buffer_size = ASR_WS_BUFFER_SIZE,
        .task_stack = ASR_WS_TASK_STACK,
        .disable_auto_reconnect = true,
        .network_timeout_ms = ASR_CONNECT_WAIT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    client = esp_websocket_client_init(&ws_cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to create WebSocket client");
        err = ESP_FAIL;
        goto cleanup;
    }

    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, asr_websocket_event_handler,
                                  &session);

    ESP_LOGI(TAG, "Connecting to ASR service: %s", url);
    err = esp_websocket_client_start(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client: %s", esp_err_to_name(err));
        goto cleanup;
    }

    bits = xEventGroupWaitBits(session.events, ASR_EVT_CONNECTED | ASR_EVT_FAILED, pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(ASR_CONNECT_WAIT_MS));
    if (!(bits & ASR_EVT_CONNECTED)) {
        ESP_LOGE(TAG, "ASR WebSocket did not connect");
        err = ESP_ERR_TIMEOUT;
        goto cleanup;
    }

    /* 1. Announce the audio format before any samples go out. */
    err = asr_send_frame(client, ASR_MSG_FULL_CLIENT, ASR_SERIAL_JSON, 1, true, config_payload,
                         strlen(config_payload));
    if (err != ESP_OK) {
        goto cleanup;
    }

    /* 2. Stream captured PCM as it arrives. */
    while (elapsed_ms < ASR_MAX_CAPTURE_MS) {
        size_t bytes_read = 0;
        esp_err_t read_err;

        if (xEventGroupGetBits(session.events) & ASR_EVT_FAILED) {
            ESP_LOGE(TAG, "ASR session failed while streaming");
            err = ESP_FAIL;
            goto cleanup;
        }

        read_err = voice_audio_io_read(pcm_chunk, ASR_PACKET_BYTES, &bytes_read,
                                       ASR_READ_TIMEOUT_MS);
        if (read_err != ESP_OK || bytes_read == 0) {
            /* Charge the wait against the budget even when nothing was read,
             * otherwise a mic that only ever times out loops forever and the
             * listening flag never clears. */
            elapsed_ms += ASR_READ_TIMEOUT_MS;
            if (++empty_reads >= ASR_MAX_EMPTY_READS) {
                ESP_LOGE(TAG, "Microphone returned no audio %d times; aborting", empty_reads);
                err = ESP_ERR_TIMEOUT;
                goto cleanup;
            }
            if (should_continue && !should_continue()) {
                break;
            }
            continue;
        }
        empty_reads = 0;

        /* Send what was just captured before deciding to stop, so the tail of
         * the utterance is never dropped. */
        sequence++;
        err = asr_send_frame(client, ASR_MSG_AUDIO_ONLY, ASR_SERIAL_NONE, sequence, true, pcm_chunk,
                             bytes_read);
        if (err != ESP_OK) {
            goto cleanup;
        }

        /* Derive elapsed time from the samples actually captured; a partial
         * read must not be charged as a full packet. */
        elapsed_ms += (int)(bytes_read * 1000 / (VOICE_ASR_SAMPLE_RATE * 2));

        if (should_continue && !should_continue()) {
            break;
        }
    }

    /* 3. A negative sequence marks the last packet and asks for the result. */
    sequence++;
    session.final_sent = true;
    err = asr_send_frame(client, ASR_MSG_AUDIO_ONLY, ASR_SERIAL_NONE, -sequence, true, pcm_chunk, 0);
    if (err != ESP_OK) {
        goto cleanup;
    }

    ESP_LOGI(TAG, "Sent final packet after %d ms, waiting for transcript", elapsed_ms);

    bits = xEventGroupWaitBits(session.events, ASR_EVT_RESULT | ASR_EVT_FAILED, pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(ASR_RESULT_WAIT_MS));

    /* Tear the client down first: the WebSocket task writes session.text, so
     * reading it while the client is still dispatching races a late frame
     * (and frees the string out from under us). */
    esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
    esp_websocket_client_destroy(client);
    client = NULL;

    if (session.text && session.text[0]) {
        snprintf(out_text, max_text_len, "%s", session.text);
        err = ESP_OK;
    } else if (bits & ASR_EVT_FAILED) {
        err = ESP_FAIL;
    } else {
        ESP_LOGW(TAG, "No transcript returned");
        err = ESP_ERR_TIMEOUT;
    }

cleanup:
    if (client) {
        esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(client);
    }
    if (session.events) {
        vEventGroupDelete(session.events);
    }
    free(session.text);
    free(session.rx_buf);
    free(pcm_chunk);
    free(headers);
    free(config_payload);
    return err;
}

#endif /* CONFIG_CLAW_CAP_VOICE_PIPELINE_ENABLED */
