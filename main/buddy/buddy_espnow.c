#include "buddy_espnow.h"
#include "buddy_profile.h"
#include "buddy_proximity.h"
#include "buddy_crypto.h"
#include "mimi_config.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_wifi_types.h"
#include "cJSON.h"

static const char *TAG = "buddy_espnow";

/* ── Get current effective channel ──────────────────────────────── */
static uint8_t get_current_channel(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.primary;
    }
    return BUDDY_ESPNOW_CHANNEL; /* fallback when not connected */
}

/* ── Internal state ───────────────────────────────────────────── */
static QueueHandle_t s_event_queue = NULL;
static SemaphoreHandle_t s_hs_lock = NULL;
static TaskHandle_t s_beacon_task = NULL;
static bool s_running = false;
static uint8_t s_broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* ── Peer tracking (beacon dedup + handshake cooldown) ────────── */
#define PEER_TRACK_MAX 32
typedef struct {
    uint8_t  mac[6];
    char     device_id[BUDDY_DEVICE_ID_LEN];
    int8_t   rssi;
    int64_t  last_beacon_ms;
    int64_t  last_handshake_ms;
    bool     blocked;       /* rate-limited */
} peer_track_t;

static peer_track_t s_peers[PEER_TRACK_MAX] = {0};
static int s_peer_count = 0;

/* ── Handshake slots ──────────────────────────────────────────── */
static buddy_handshake_t s_handshakes[BUDDY_MAX_CONCURRENT_HS] = {0};

/* ── Peer tracking helpers ────────────────────────────────────── */
static peer_track_t *peer_find_or_add(const uint8_t *mac)
{
    for (int i = 0; i < s_peer_count; i++) {
        if (memcmp(s_peers[i].mac, mac, 6) == 0) return &s_peers[i];
    }
    if (s_peer_count >= PEER_TRACK_MAX) {
        /* Evict oldest */
        int oldest = 0;
        for (int i = 1; i < PEER_TRACK_MAX; i++) {
            if (s_peers[i].last_beacon_ms < s_peers[oldest].last_beacon_ms) oldest = i;
        }
        memset(&s_peers[oldest], 0, sizeof(peer_track_t));
        memcpy(s_peers[oldest].mac, mac, 6);
        return &s_peers[oldest];
    }
    memcpy(s_peers[s_peer_count].mac, mac, 6);
    return &s_peers[s_peer_count++];
}

static bool peer_should_handshake(const uint8_t *mac)
{
    peer_track_t *p = peer_find_or_add(mac);
    if (p->blocked) return false;
    int64_t now = esp_timer_get_time() / 1000LL;
    if (p->last_handshake_ms > 0 &&
        (now - p->last_handshake_ms) < (BUDDY_REHANDSHAKE_COOLDOWN_S * 1000LL)) {
        return false;
    }
    if ((now - p->last_beacon_ms) < BUDDY_BEACON_DEDUP_MS) {
        return false;  /* too soon */
    }
    return true;
}

/* ── Handshake slot management ────────────────────────────────── */
static buddy_handshake_t *hs_slot_alloc(void)
{
    xSemaphoreTake(s_hs_lock, portMAX_DELAY);
    for (int i = 0; i < BUDDY_MAX_CONCURRENT_HS; i++) {
        if (!s_handshakes[i].active) {
            memset(&s_handshakes[i], 0, sizeof(buddy_handshake_t));
            s_handshakes[i].active = true;
            xSemaphoreGive(s_hs_lock);
            return &s_handshakes[i];
        }
    }
    xSemaphoreGive(s_hs_lock);
    return NULL;
}

static buddy_handshake_t *hs_slot_find_by_mac(const uint8_t *mac)
{
    xSemaphoreTake(s_hs_lock, portMAX_DELAY);
    for (int i = 0; i < BUDDY_MAX_CONCURRENT_HS; i++) {
        if (s_handshakes[i].active && memcmp(s_handshakes[i].peer_mac, mac, 6) == 0) {
            xSemaphoreGive(s_hs_lock);
            return &s_handshakes[i];
        }
    }
    xSemaphoreGive(s_hs_lock);
    return NULL;
}

static void hs_slot_free(buddy_handshake_t *hs)
{
    xSemaphoreTake(s_hs_lock, portMAX_DELAY);
    memset(hs, 0, sizeof(*hs));
    xSemaphoreGive(s_hs_lock);
}

/* ── Frame sending helpers ────────────────────────────────────── */
static esp_err_t peer_ensure(const uint8_t *mac)
{
    if (memcmp(mac, s_broadcast_mac, 6) == 0) return ESP_OK;

    /* channel=0 means "use current home channel" — avoids mismatch on reconnect */
    esp_now_peer_info_t peer_info = {0};
    memcpy(peer_info.peer_addr, mac, 6);
    peer_info.channel = 0;
    peer_info.ifidx = WIFI_IF_STA;

    if (!esp_now_is_peer_exist(mac)) {
        esp_err_t err = esp_now_add_peer(&peer_info);
        if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
            ESP_LOGW(TAG, "add_peer %02x:%02x:%02x:%02x:%02x:%02x failed: %s",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], esp_err_to_name(err));
            return err;
        }
        ESP_LOGI(TAG, "Unicast peer added: %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return ESP_OK;
}

/* ── Send status callback ──────────────────────────────────────── */
static void espnow_send_cb(const wifi_tx_info_t *tx_info, esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS && tx_info && tx_info->des_addr) {
        ESP_LOGW(TAG, "Send to %02x:%02x:%02x:%02x:%02x:%02x failed (status=%d)",
                 tx_info->des_addr[0], tx_info->des_addr[1], tx_info->des_addr[2],
                 tx_info->des_addr[3], tx_info->des_addr[4], tx_info->des_addr[5],
                 (int)status);
    }
}

static esp_err_t espnow_send(const uint8_t *mac, const uint8_t *data, size_t len)
{
    esp_err_t err = peer_ensure(mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "peer_ensure failed: %s", esp_err_to_name(err));
        return err;
    }
    return esp_now_send(mac, data, len);
}

static esp_err_t send_handshake_request(const uint8_t *peer_mac, const char *device_id)
{
    /* Allocate handshake slot */
    buddy_handshake_t *hs = hs_slot_alloc();
    if (!hs) return ESP_ERR_NO_MEM;

    memcpy(hs->peer_mac, peer_mac, 6);
    snprintf((char *)hs->peer_device_id, sizeof(hs->peer_device_id), "%s", device_id);
    hs->timestamp_start = esp_timer_get_time() / 1000;

    /* Build frame — PSK-based, no ephemeral keys needed */
    buddy_hs_req_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = BUDDY_FRAME_TYPE_HS_REQ;
    esp_read_mac(frame.device_id, ESP_MAC_WIFI_STA);
    frame.timestamp = hs->timestamp_start;
    for (int i = 0; i < 16; i++) frame.nonce[i] = (uint8_t)esp_random();

    /* Store nonce for session key derivation when HS_RESP arrives */
    memcpy(hs->nonce, frame.nonce, 16);

    esp_err_t err = espnow_send(peer_mac, (uint8_t *)&frame, sizeof(frame));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "HS_REQ send failed: %s", esp_err_to_name(err));
        hs_slot_free(hs);
        return err;
    }

    ESP_LOGI(TAG, "HS_REQ sent to %02x:%02x:%02x:%02x:%02x:%02x",
             peer_mac[0], peer_mac[1], peer_mac[2],
             peer_mac[3], peer_mac[4], peer_mac[5]);
    return ESP_OK;
}

static esp_err_t send_encrypted_profile(const uint8_t *peer_mac, buddy_handshake_t *hs)
{
    /* Serialize own profile to JSON */
    buddy_profile_t profile;
    buddy_profile_get(&profile);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "dn", profile.display_name);
    cJSON_AddStringToObject(root, "bi", profile.bio);
    cJSON_AddStringToObject(root, "tg", profile.tags);
    cJSON_AddStringToObject(root, "vb", profile.vibe);
    cJSON_AddStringToObject(root, "ot", profile.open_to);
    cJSON_AddStringToObject(root, "cp", profile.contact_phone);
    cJSON_AddStringToObject(root, "ce", profile.contact_email);

    const buddy_identity_t *id = buddy_identity_get();
    cJSON_AddStringToObject(root, "did", id->device_id);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return ESP_ERR_NO_MEM;

    size_t plain_len = strlen(json_str);
    /* Max encrypted per fragment: 250 - 3 header - 16 auth = 231 bytes */
    #define PROFILE_FRAG_PAYLOAD 231

    uint8_t plain_buf[BUDDY_PROFILE_MAX_BYTES];
    memcpy(plain_buf, json_str, plain_len);
    free(json_str);

    int total_frags = (int)((plain_len + PROFILE_FRAG_PAYLOAD - 1) / PROFILE_FRAG_PAYLOAD);
    if (total_frags > BUDDY_MAX_FRAGMENTS) total_frags = BUDDY_MAX_FRAGMENTS;

    uint8_t frame_buf[250];
    for (int seq = 0; seq < total_frags; seq++) {
        size_t chunk_start = seq * PROFILE_FRAG_PAYLOAD;
        size_t chunk_len = plain_len - chunk_start;
        if (chunk_len > PROFILE_FRAG_PAYLOAD) chunk_len = PROFILE_FRAG_PAYLOAD;

        memset(frame_buf, 0, sizeof(frame_buf));
        frame_buf[0] = BUDDY_FRAME_TYPE_PROFILE;
        frame_buf[1] = (uint8_t)seq;
        frame_buf[2] = (uint8_t)total_frags;

        uint8_t auth_tag[16];
        int enc_len = buddy_crypto_encrypt(
            plain_buf + chunk_start, chunk_len,
            hs->aes_key, hs->aes_iv,
            frame_buf + 3, auth_tag);

        if (enc_len < 0) {
            ESP_LOGE(TAG, "Encrypt fragment %d failed", seq);
            return ESP_FAIL;
        }
        memcpy(frame_buf + 3 + enc_len, auth_tag, 16);

        esp_err_t err = espnow_send(peer_mac, frame_buf, 3 + enc_len + 16);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Profile frag %d send failed", seq);
            return err;
        }
    }

    ESP_LOGI(TAG, "Profile sent to peer (%d bytes, %d frags)", (int)plain_len, total_frags);
    return ESP_OK;
}

/* ── ESP-NOW receive callback ─────────────────────────────────── */
static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (!data || len < 1) return;

    const uint8_t *src_mac = recv_info->src_addr;
    int8_t rssi = recv_info->rx_ctrl ? recv_info->rx_ctrl->rssi : -90;
    uint8_t frame_type = data[0];

    /* Feed proximity for all frames */
    buddy_proximity_feed(rssi);

    ESP_LOGD(TAG, "RX type=0x%02x len=%d from %02x:%02x:%02x:%02x:%02x:%02x rssi=%d",
             frame_type, len,
             src_mac[0], src_mac[1], src_mac[2],
             src_mac[3], src_mac[4], src_mac[5], rssi);

    switch (frame_type) {

    case BUDDY_FRAME_TYPE_BEACON: {
        if (len < (int)sizeof(buddy_beacon_frame_t)) return;
        const buddy_beacon_frame_t *b = (const buddy_beacon_frame_t *)data;

        /* Skip if flag bit2 = private mode */
        if (b->flags & 0x04) return;

        /* Update peer tracking */
        peer_track_t *p = peer_find_or_add(src_mac);
        p->rssi = rssi;

        /* Should we initiate handshake? */
        if (!peer_should_handshake(src_mac)) return;
        if (buddy_privacy_get() == BUDDY_MODE_PRIVATE) return;

        /* Check if accepting handshakes (bit0) */
        if (!(b->flags & 0x01)) return;

        ESP_LOGI(TAG, "Beacon from %02x:%02x:%02x:%02x:%02x:%02x flags=0x%02x rssi=%d",
                 src_mac[0], src_mac[1], src_mac[2],
                 src_mac[3], src_mac[4], src_mac[5], b->flags, rssi);

        char did[18];
        snprintf(did, sizeof(did), "%02x:%02x:%02x:%02x:%02x:%02x",
                 b->device_id[0], b->device_id[1], b->device_id[2],
                 b->device_id[3], b->device_id[4], b->device_id[5]);

        int64_t now = esp_timer_get_time() / 1000LL;
        p->last_beacon_ms = now;
        p->last_handshake_ms = now;
        strncpy(p->device_id, did, sizeof(p->device_id) - 1);
        send_handshake_request(src_mac, did);
        break;
    }

    case BUDDY_FRAME_TYPE_HS_REQ: {
        if (len < (int)sizeof(buddy_hs_req_frame_t)) return;
        const buddy_hs_req_frame_t *req = (const buddy_hs_req_frame_t *)data;

        ESP_LOGI(TAG, "HS_REQ from %02x:%02x:%02x:%02x:%02x:%02x",
                 src_mac[0], src_mac[1], src_mac[2],
                 src_mac[3], src_mac[4], src_mac[5]);

        if (buddy_privacy_get() == BUDDY_MODE_PRIVATE) return;

        /* Check for simultaneous handshake */
        buddy_handshake_t *hs = hs_slot_find_by_mac(src_mac);
        bool simultaneous = (hs != NULL);
        const uint8_t *canonical_nonce;

        if (simultaneous) {
            ESP_LOGI(TAG, "HS_REQ: simultaneous handshake, resolving...");
            /* Lower MAC wins as canonical initiator */
            uint8_t my_mac[6];
            esp_read_mac(my_mac, ESP_MAC_WIFI_STA);
            int mac_cmp = memcmp(my_mac, src_mac, 6);
            if (mac_cmp < 0) {
                /* We're the winner — our nonce is canonical */
                canonical_nonce = hs->nonce;
                ESP_LOGI(TAG, "Simul: we are initiator (lower MAC)");
            } else {
                /* They're the winner — their nonce is canonical */
                canonical_nonce = req->nonce;
                ESP_LOGI(TAG, "Simul: peer is initiator (lower MAC)");
            }
        } else {
            canonical_nonce = req->nonce;
        }

        /* Derive session keys from PSK + canonical nonce */
        uint8_t aes_key[16], aes_iv[12];
        if (buddy_crypto_derive_session_keys(canonical_nonce, aes_key, aes_iv) != ESP_OK) {
            ESP_LOGE(TAG, "Session key derivation failed");
            return;
        }

        if (!simultaneous) {
            hs = hs_slot_alloc();
            if (!hs) return;
        }

        memcpy(hs->peer_mac, src_mac, 6);
        memcpy(hs->nonce, canonical_nonce, 16);
        memcpy(hs->aes_key, aes_key, 16);
        memcpy(hs->aes_iv, aes_iv, 12);
        hs->timestamp_start = esp_timer_get_time() / 1000;

        /* Send HS_RESP echoing the canonical nonce */
        buddy_hs_resp_frame_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.type = BUDDY_FRAME_TYPE_HS_RESP;
        esp_read_mac(resp.device_id, ESP_MAC_WIFI_STA);
        memcpy(resp.req_nonce, canonical_nonce, 16);

        espnow_send(src_mac, (uint8_t *)&resp, sizeof(resp));

        /* Send our encrypted profile */
        send_encrypted_profile(src_mac, hs);
        hs->profile_sent = true;

        ESP_LOGI(TAG, "HS_RESP + profile sent%s",
                 simultaneous ? " (simul)" : "");
        break;
    }

    case BUDDY_FRAME_TYPE_HS_RESP: {
        if (len < (int)sizeof(buddy_hs_resp_frame_t)) return;
        const buddy_hs_resp_frame_t *resp = (const buddy_hs_resp_frame_t *)data;

        ESP_LOGI(TAG, "HS_RESP from %02x:%02x:%02x:%02x:%02x:%02x",
                 src_mac[0], src_mac[1], src_mac[2],
                 src_mac[3], src_mac[4], src_mac[5]);

        buddy_handshake_t *hs = hs_slot_find_by_mac(src_mac);
        if (!hs) {
            ESP_LOGW(TAG, "HS_RESP ignored: no active handshake slot");
            return;
        }

        /* Derive session keys from PSK + canonical nonce (echoed back) */
        buddy_crypto_derive_session_keys(resp->req_nonce, hs->aes_key, hs->aes_iv);
        memcpy(hs->nonce, resp->req_nonce, 16);

        /* Send our encrypted profile (unless already sent in simultaneous path) */
        if (!hs->profile_sent) {
            send_encrypted_profile(src_mac, hs);
            hs->profile_sent = true;
        }

        ESP_LOGI(TAG, "Received HS_RESP, session keys derived%s",
                 hs->profile_sent ? " (already sent)" : "");
        break;
    }

    case BUDDY_FRAME_TYPE_PROFILE: {
        if (len < 3 + 16) return; /* type + seq + total + auth_tag */
        uint8_t seq = data[1];
        uint8_t total = data[2];
        size_t enc_len = (size_t)(len - 3 - 16);

        ESP_LOGI(TAG, "Profile frag %d/%d from %02x:%02x:%02x:%02x:%02x:%02x",
                 seq + 1, total,
                 src_mac[0], src_mac[1], src_mac[2],
                 src_mac[3], src_mac[4], src_mac[5]);

        buddy_handshake_t *hs = hs_slot_find_by_mac(src_mac);
        if (!hs) {
            ESP_LOGW(TAG, "Profile frag ignored: no active handshake");
            return;
        }

        /* Decrypt */
        uint8_t plain[PROFILE_FRAG_PAYLOAD];
        int plain_len = buddy_crypto_decrypt(
            data + 3, enc_len,
            hs->aes_key, hs->aes_iv,
            data + 3 + enc_len,
            plain, sizeof(plain));

        if (plain_len < 0) {
            ESP_LOGW(TAG, "Profile decrypt failed");
            return;
        }

        /* Reassemble */
        size_t offset = seq * PROFILE_FRAG_PAYLOAD;
        if (offset + plain_len > sizeof(hs->reassembly_buf)) {
            ESP_LOGW(TAG, "Profile too large");
            return;
        }
        memcpy(hs->reassembly_buf + offset, plain, plain_len);
        hs->reassembly_len = offset + plain_len;
        hs->pending_frags = total;

        /* Check if complete */
        if (seq + 1 >= total) {
            hs->profile_complete = true;

            /* Post event */
            buddy_event_t evt = {0};
            evt.type = BUDDY_EVT_PROFILE_READY;
            memcpy(evt.peer_mac, src_mac, 6);
            evt.rssi = rssi;
            evt.proximity = buddy_proximity_classify();

            /* Parse profile JSON */
            char *json_copy = strndup((const char *)hs->reassembly_buf, hs->reassembly_len);
            if (json_copy) {
                cJSON *root = cJSON_Parse(json_copy);
                if (root) {
                    cJSON *dn = cJSON_GetObjectItem(root, "dn");
                    if (dn) snprintf(evt.peer_profile.display_name,
                                     sizeof(evt.peer_profile.display_name),
                                     "%s", dn->valuestring);
                    cJSON *bi = cJSON_GetObjectItem(root, "bi");
                    if (bi) snprintf(evt.peer_profile.bio,
                                     sizeof(evt.peer_profile.bio),
                                     "%s", bi->valuestring);
                    cJSON *tg = cJSON_GetObjectItem(root, "tg");
                    if (tg) snprintf(evt.peer_profile.tags,
                                     sizeof(evt.peer_profile.tags),
                                     "%s", tg->valuestring);
                    cJSON *vb = cJSON_GetObjectItem(root, "vb");
                    if (vb) snprintf(evt.peer_profile.vibe,
                                     sizeof(evt.peer_profile.vibe),
                                     "%s", vb->valuestring);
                    cJSON *ot = cJSON_GetObjectItem(root, "ot");
                    if (ot) snprintf(evt.peer_profile.open_to,
                                     sizeof(evt.peer_profile.open_to),
                                     "%s", ot->valuestring);
                    cJSON *cp = cJSON_GetObjectItem(root, "cp");
                    if (cp) snprintf(evt.peer_profile.contact_phone,
                                     sizeof(evt.peer_profile.contact_phone),
                                     "%s", cp->valuestring);
                    cJSON *ce = cJSON_GetObjectItem(root, "ce");
                    if (ce) snprintf(evt.peer_profile.contact_email,
                                     sizeof(evt.peer_profile.contact_email),
                                     "%s", ce->valuestring);
                    cJSON *did = cJSON_GetObjectItem(root, "did");
                    if (did) {
                        snprintf(evt.peer_device_id, sizeof(evt.peer_device_id),
                                 "%s", did->valuestring);
                        snprintf((char *)hs->peer_device_id, sizeof(hs->peer_device_id),
                                 "%s", did->valuestring);
                    }
                    cJSON_Delete(root);
                }
                free(json_copy);
            }

            evt.peer_profile_valid = true;

            if (s_event_queue) {
                xQueueSend(s_event_queue, &evt, pdMS_TO_TICKS(100));
            }

            hs_slot_free(hs);
            ESP_LOGI(TAG, "Profile received from %s (name=%s)",
                     evt.peer_device_id, evt.peer_profile.display_name);
        }
        break;
    }

    default:
        break;
    }
}

/* ── Beacon TX task ───────────────────────────────────────────── */
static void beacon_tx_task(void *arg)
{
    ESP_LOGI(TAG, "Beacon TX task started (period=%d±%dms, ch=%d)",
             BUDDY_BEACON_PERIOD_MS, BUDDY_BEACON_JITTER_MS, BUDDY_ESPNOW_CHANNEL);

    while (s_running) {
        if (buddy_privacy_get() == BUDDY_MODE_PUBLIC) {
            buddy_espnow_send_beacon();
        }

        /* Randomized interval */
        int jitter = (int)(esp_random() % (BUDDY_BEACON_JITTER_MS * 2 + 1)) - BUDDY_BEACON_JITTER_MS;
        int delay = BUDDY_BEACON_PERIOD_MS + jitter;
        if (delay < 100) delay = 100;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }

    vTaskDelete(NULL);
}

/* ── Public API ───────────────────────────────────────────────── */
esp_err_t buddy_espnow_send_beacon(void)
{
    buddy_beacon_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.version = 1;
    frame.type = BUDDY_FRAME_TYPE_BEACON;

    const buddy_identity_t *id = buddy_identity_get();
    /* Parse device_id "aa:bb:cc:dd:ee:ff" -> bytes */
    sscanf(id->device_id, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &frame.device_id[0], &frame.device_id[1], &frame.device_id[2],
           &frame.device_id[3], &frame.device_id[4], &frame.device_id[5]);

    buddy_profile_t profile;
    buddy_profile_get(&profile);
    memcpy(frame.profile_hash, profile.profile_hash, 8);

    frame.rssi_cal = 0;
    for (int i = 0; i < 4; i++) frame.nonce[i] = (uint8_t)esp_random();
    frame.flags = 0x01;  /* accepting handshakes */
    if (buddy_privacy_get() == BUDDY_MODE_PRIVATE) frame.flags |= 0x04;

    return esp_now_send(s_broadcast_mac, (uint8_t *)&frame, sizeof(frame));
}

esp_err_t buddy_espnow_init(void)
{
    /* Create event queue */
    s_event_queue = xQueueCreate(4, sizeof(buddy_event_t));
    if (!s_event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    s_hs_lock = xSemaphoreCreateMutex();
    if (!s_hs_lock) {
        ESP_LOGE(TAG, "Failed to create handshake lock");
        return ESP_ERR_NO_MEM;
    }

    /* Init ESP-NOW */
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Disable WiFi power save — ESP-NOW triggers pm_update_by_connectionless_status
     * which can deadlock the PHY task under WIFI_PS_MIN_MODEM */
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* Set PMK for encrypted ESP-NOW (must be exactly 16 bytes) */
    err = esp_now_set_pmk((const uint8_t *)BUDDY_ESPNOW_PMK);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_set_pmk failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "ESP-NOW PMK set");
    }

    /* Register callbacks */
    esp_now_register_send_cb(espnow_send_cb);
    esp_now_register_recv_cb(espnow_recv_cb);

    ESP_LOGI(TAG, "ESP-NOW initialized (PMK=%s)", BUDDY_ESPNOW_PMK);
    return ESP_OK;
}

esp_err_t buddy_espnow_start(void)
{
    if (s_running) return ESP_OK;

    s_running = true;

    uint8_t ch = get_current_channel();
    ESP_LOGI(TAG, "Starting ESP-NOW on ch %d (WiFi %s)",
             ch, (ch != BUDDY_ESPNOW_CHANNEL) ? "channel differs from default" : "matched default");

    /* Create beacon TX task on Core 0 */
    BaseType_t ret = xTaskCreatePinnedToCore(
        beacon_tx_task, "buddy_beacon",
        3072, NULL, 4, &s_beacon_task, 0);

    if (ret != pdPASS) {
        s_running = false;
        ESP_LOGE(TAG, "Failed to create beacon task");
        return ESP_FAIL;
    }

    /* Add broadcast peer (channel=0 = use current home channel) */
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_broadcast_mac, 6);
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&peer);  /* ok if already exists */

    ESP_LOGI(TAG, "ESP-NOW started (beacon on ch %d)", ch);
    return ESP_OK;
}

esp_err_t buddy_espnow_stop(void)
{
    s_running = false;
    if (s_beacon_task) {
        vTaskDelete(s_beacon_task);
        s_beacon_task = NULL;
    }
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    ESP_LOGI(TAG, "ESP-NOW stopped");
    return ESP_OK;
}

QueueHandle_t buddy_espnow_get_event_queue(void)
{
    return s_event_queue;
}
