#include "buddy_agent.h"
#include "buddy_espnow.h"
#include "buddy_contacts.h"
#include "buddy_profile.h"
#include "buddy_proximity.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "llm/llm_proxy.h"
#include "wifi/wifi_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cJSON.h"
#include "led_strip.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"

static const char *TAG = "buddy_agent";

/* ── LED driver (WS2812B on GPIO48) ──────────────────────────── */
#define BUDDY_LED_GPIO           48
#define BUDDY_LED_RMT_RES_HZ     (10 * 1000 * 1000)

static led_strip_handle_t s_led_strip = NULL;

static esp_err_t led_init(void)
{
    if (s_led_strip) return ESP_OK;

    led_strip_config_t cfg = {
        .strip_gpio_num = BUDDY_LED_GPIO,
        .max_leds = 3,  /* 3 LEDs for proximity ring */
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };

    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = BUDDY_LED_RMT_RES_HZ,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };

    return led_strip_new_rmt_device(&cfg, &rmt_cfg, &s_led_strip);
}

static void led_set_rgb(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led_strip) return;
    led_strip_set_pixel(s_led_strip, idx, r, g, b);
    led_strip_refresh(s_led_strip);
}

esp_err_t buddy_led_set(buddy_led_pattern_t pattern)
{
    led_init();

    switch (pattern) {
    case BUDDY_LED_PATTERN_OFF:
        for (int i = 0; i < 3; i++) led_set_rgb(i, 0, 0, 0);
        break;
    case BUDDY_LED_PATTERN_BLUE_SLOW:
        for (int i = 0; i < 3; i++) led_set_rgb(i, 0, 0, 32);
        break;
    case BUDDY_LED_PATTERN_AMBER:
        for (int i = 0; i < 3; i++) led_set_rgb(i, 255, 64, 0);
        break;
    case BUDDY_LED_PATTERN_GREEN_FAST:
        for (int i = 0; i < 3; i++) led_set_rgb(i, 0, 255, 0);
        break;
    case BUDDY_LED_PATTERN_AMBER_BRIEF:
        for (int i = 0; i < 3; i++) led_set_rgb(i, 255, 64, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
        for (int i = 0; i < 3; i++) led_set_rgb(i, 0, 0, 0);
        break;
    }
    return ESP_OK;
}

/* ── LLM match scoring ────────────────────────────────────────── */
static esp_err_t cloud_match_score(const buddy_profile_t *self,
                                   const buddy_profile_t *peer,
                                   buddy_contact_status_t status,
                                   buddy_proximity_t proximity,
                                   char *icebreaker_out, size_t icebreaker_size,
                                   float *score_out,
                                   char *shared_out, size_t shared_size)
{
    /* Build system prompt */
    static const char *system_prompt =
        "你是一个近距离社交设备的匹配助手。\n"
        "两个人刚刚在街上擦肩而过。你的任务：\n"
        "1. 评估他们兴趣和气场的匹配度（0.0–1.0）\n"
        "2. 找出最引人注目的共同点\n"
        "3. 以对方（Peer）的口吻，写一句温暖、具体的破冰语（1–2句话）\n\n"
        "破冰语要求：\n"
        "- 破冰语是对方发给自己的，即用Peer的身份对Self说话\n"
        "- 结合双方的名字、身份简介、状态、交友意愿和共同兴趣点来写\n"
        "- 自然地提到具体的共同标签或话题，让对方感觉是专门为他们定制的\n"
        "- 不要写\"你们似乎有很多共同点！\"这种泛泛的话\n\n"
        "只返回合法的JSON，不要前缀说明：\n"
        "{\"match_score\":0.84,\"shared_interests\":[\"标签1\",\"标签2\"],\"icebreaker\":\"...\",\"connection_reason\":\"...\"}";

    /* Build user message with both profiles */
    char user_msg[2048];
    snprintf(user_msg, sizeof(user_msg),
        "自己的档案:\n"
        "名字: %s\n"
        "简介: %s\n"
        "标签: %s\n"
        "状态: %s\n"
        "交友意愿: %s\n\n"
        "对方的档案:\n"
        "名字: %s\n"
        "简介: %s\n"
        "标签: %s\n"
        "状态: %s\n"
        "交友意愿: %s\n\n"
        "Context: contact_status=%s, proximity=%s",
        self->display_name, self->bio, self->tags, self->vibe, self->open_to,
        peer->display_name, peer->bio, peer->tags, peer->vibe, peer->open_to,
        status == BUDDY_CONTACT_NEW ? "NEW" :
        status == BUDDY_CONTACT_KNOWN ? "KNOWN" : "RECENT",
        buddy_proximity_str(proximity));

    /* Build messages JSON */
    cJSON *messages = cJSON_CreateArray();
    cJSON *um = cJSON_CreateObject();
    cJSON_AddStringToObject(um, "role", "user");
    cJSON_AddStringToObject(um, "content", user_msg);
    cJSON_AddItemToArray(messages, um);
    char *msgs_str = cJSON_PrintUnformatted(messages);
    cJSON_Delete(messages);
    if (!msgs_str) return ESP_ERR_NO_MEM;

    /* Call LLM — response buffer on heap to limit stack usage */
    char *response = heap_caps_calloc(1, 4096, MALLOC_CAP_SPIRAM);
    if (!response) return ESP_ERR_NO_MEM;

    esp_err_t err = llm_chat(system_prompt, msgs_str, response, 4096);
    free(msgs_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LLM match call failed: %s", esp_err_to_name(err));
        free(response);
        return err;
    }

    /* Parse JSON response */
    cJSON *root = cJSON_Parse(response);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse LLM response: %.100s", response);
        free(response);
        return ESP_FAIL;
    }
    free(response);

    cJSON *ms = cJSON_GetObjectItem(root, "match_score");
    cJSON *ib = cJSON_GetObjectItem(root, "icebreaker");
    cJSON *si = cJSON_GetObjectItem(root, "shared_interests");
    cJSON *cr = cJSON_GetObjectItem(root, "connection_reason");

    if (ms && cJSON_IsNumber(ms)) *score_out = (float)ms->valuedouble;
    else *score_out = 0.5f;

    if (ib && cJSON_IsString(ib)) {
        snprintf(icebreaker_out, icebreaker_size, "%s", ib->valuestring);
    } else {
        snprintf(icebreaker_out, icebreaker_size, "You two might have a lot to talk about!");
    }

    if (si && cJSON_IsArray(si)) {
        char *joined = cJSON_PrintUnformatted(si);
        if (joined) {
            snprintf(shared_out, shared_size, "%s", joined);
            free(joined);
        }
    } else if (cr && cJSON_IsString(cr)) {
        snprintf(shared_out, shared_size, "%s", cr->valuestring);
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Match score: %.2f, icebreaker: %.60s", *score_out, icebreaker_out);
    return ESP_OK;
}

/* ── Contact processing task ──────────────────────────────────── */
static void buddy_contact_task(void *arg)
{
    QueueHandle_t evt_queue = buddy_espnow_get_event_queue();
    if (!evt_queue) {
        ESP_LOGE(TAG, "No event queue, aborting");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Buddy contact task started on Core %d", xPortGetCoreID());

    while (1) {
        buddy_event_t evt;
        if (xQueueReceive(evt_queue, &evt, portMAX_DELAY) != pdTRUE) continue;

        if (evt.type != BUDDY_EVT_PROFILE_READY || !evt.peer_profile_valid) {
            continue;
        }

        const char *peer_id = evt.peer_device_id[0] ? evt.peer_device_id : "unknown";
        ESP_LOGI(TAG, "Processing contact: %s (rssi=%d, prox=%s)",
                 peer_id, evt.rssi, buddy_proximity_str(evt.proximity));

        /* Check contact status */
        buddy_contact_status_t cstat = buddy_contacts_check(peer_id);

        /* LED feedback */
        switch (cstat) {
        case BUDDY_CONTACT_NEW:
            buddy_led_set(BUDDY_LED_PATTERN_GREEN_FAST);
            break;
        case BUDDY_CONTACT_KNOWN:
            buddy_led_set(BUDDY_LED_PATTERN_BLUE_SLOW);
            break;
        case BUDDY_CONTACT_RECENT:
            /* Silent — met within 24h, don't spam */
            ESP_LOGI(TAG, "Recent contact, skipping");
            continue;
        }

        /* Store contact locally */
        buddy_contact_record_t rec = {0};
        snprintf(rec.peer_id, sizeof(rec.peer_id), "%s", peer_id);
        snprintf(rec.display_name, sizeof(rec.display_name), "%s",
                 evt.peer_profile.display_name);
        snprintf(rec.tags, sizeof(rec.tags), "%s", evt.peer_profile.tags);
        snprintf(rec.bio, sizeof(rec.bio), "%s", evt.peer_profile.bio);
        snprintf(rec.contact_phone, sizeof(rec.contact_phone), "%s",
                 evt.peer_profile.contact_phone);
        snprintf(rec.contact_email, sizeof(rec.contact_email), "%s",
                 evt.peer_profile.contact_email);
        rec.cloud_synced = false;
        buddy_contacts_upsert(&rec);

        /* Cloud match scoring (WiFi required) */
        if (!wifi_manager_is_connected()) {
            ESP_LOGW(TAG, "No WiFi — skipping cloud match for %s", peer_id);
            buddy_led_set(BUDDY_LED_PATTERN_AMBER_BRIEF);
            continue;
        }

        buddy_profile_t self;
        buddy_profile_get(&self);

        float score = 0.5f;
        char icebreaker[256] = {0};
        char shared[128] = {0};

        esp_err_t match_err = cloud_match_score(
            &self, &evt.peer_profile, cstat, evt.proximity,
            icebreaker, sizeof(icebreaker),
            &score, shared, sizeof(shared));

        if (match_err == ESP_OK) {
            /* Update contact record */
            buddy_contacts_update_match(peer_id, score, icebreaker, shared);

            /* Send notification via configured channel (or auto-fallback to last active) */
            char notify_channel[16] = MIMI_CHAN_SYSTEM;
            char notify_chat_id[96] = "buddy";
            {
                nvs_handle_t nvs;
                if (nvs_open(MIMI_NVS_FEATURE, NVS_READONLY, &nvs) == ESP_OK) {
                    size_t len = sizeof(notify_channel);
                    esp_err_t ch_err = nvs_get_str(nvs, MIMI_NVS_KEY_BUDDY_NOTIFY_CHANNEL,
                                                   notify_channel, &len);
                    len = sizeof(notify_chat_id);
                    esp_err_t id_err = nvs_get_str(nvs, MIMI_NVS_KEY_BUDDY_NOTIFY_CHAT_ID,
                                                   notify_chat_id, &len);
                    /* Fallback to last active conversation channel */
                    if (ch_err != ESP_OK) {
                        len = sizeof(notify_channel);
                        if (nvs_get_str(nvs, MIMI_NVS_KEY_LAST_SRC_CHANNEL,
                                        notify_channel, &len) != ESP_OK) {
                            strncpy(notify_channel, MIMI_CHAN_SYSTEM, sizeof(notify_channel) - 1);
                        }
                    }
                    if (id_err != ESP_OK) {
                        len = sizeof(notify_chat_id);
                        if (nvs_get_str(nvs, MIMI_NVS_KEY_LAST_SRC_CHAT_ID,
                                        notify_chat_id, &len) != ESP_OK) {
                            strncpy(notify_chat_id, "buddy", sizeof(notify_chat_id) - 1);
                        }
                    }
                    nvs_close(nvs);
                }
            }

            mimi_msg_t sys_msg = {0};
            strncpy(sys_msg.channel, notify_channel, sizeof(sys_msg.channel) - 1);
            strncpy(sys_msg.chat_id, notify_chat_id, sizeof(sys_msg.chat_id) - 1);
            strncpy(sys_msg.type, "text", sizeof(sys_msg.type) - 1);

            char sys_body[2048];
            snprintf(sys_body, sizeof(sys_body),
                "\xF0\x9F\x94\x97 Buddy Match!\n\n"
                "Peer: %s\n"
                "Score: %.0f%%\n"
                "Icebreaker: %s\n"
                "Phone: %s\n"
                "Email: %s",
                evt.peer_profile.display_name,
                score * 100,
                icebreaker,
                evt.peer_profile.contact_phone,
                evt.peer_profile.contact_email);

            sys_msg.payload.text = strdup(sys_body);
            if (sys_msg.payload.text) {
                if (message_bus_push_outbound(&sys_msg) != ESP_OK) {
                    free(sys_msg.payload.text);
                }
            }

            ESP_LOGI(TAG, "Match complete: %s (%.2f)", peer_id, score);
        } else {
            ESP_LOGW(TAG, "Cloud match failed for %s, retry once in 30s...", peer_id);
            /* Simple retry after delay */
            vTaskDelay(pdMS_TO_TICKS(30000));
            match_err = cloud_match_score(
                &self, &evt.peer_profile, cstat, evt.proximity,
                icebreaker, sizeof(icebreaker),
                &score, shared, sizeof(shared));
            if (match_err == ESP_OK) {
                buddy_contacts_update_match(peer_id, score, icebreaker, shared);
            } else {
                ESP_LOGW(TAG, "Cloud match retry failed, dropping");
            }
        }

        buddy_led_set(BUDDY_LED_PATTERN_OFF);
        vTaskDelay(pdMS_TO_TICKS(100));  /* brief gap */
    }
}

/* ── External match trigger (for debug / WiFi onboarding) ─────── */
esp_err_t buddy_match_trigger(const buddy_profile_t *self,
                              const buddy_profile_t *peer,
                              buddy_contact_status_t status,
                              buddy_proximity_t proximity)
{
    if (!self || !peer) return ESP_ERR_INVALID_ARG;
    if (!wifi_manager_is_connected()) return ESP_ERR_INVALID_STATE;

    float score = 0.5f;
    char icebreaker[256] = {0};
    char shared[128] = {0};

    return cloud_match_score(self, peer, status, proximity,
                             icebreaker, sizeof(icebreaker),
                             &score, shared, sizeof(shared));
}

/* ── Init / Start ─────────────────────────────────────────────── */
esp_err_t buddy_agent_init(void)
{
    esp_err_t err = led_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED init failed (non-fatal): %s", esp_err_to_name(err));
    }
    buddy_led_set(BUDDY_LED_PATTERN_OFF);
    return ESP_OK;
}

esp_err_t buddy_agent_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        buddy_contact_task, "buddy_contact",
        16384, NULL, 5, NULL, 1);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create contact task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Buddy agent started on Core 1");
    return ESP_OK;
}
