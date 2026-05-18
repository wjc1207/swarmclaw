#include "buddy_ble.h"
#include "buddy_profile.h"
#include "buddy_proximity.h"
#include "buddy_contacts.h"
#include "mimi_config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "cJSON.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "esp_bt.h"

static const char *TAG = "buddy_ble";

/* ── Peer tracking (beacon dedup + handshake cooldown) ────────── */
#define PEER_TRACK_MAX 32

typedef struct {
    uint8_t  mac[6];
    char     device_id[18];
    int8_t   rssi;
    int64_t  last_ad_ms;
    int64_t  last_conn_ms;
    bool     blocked;
} peer_track_t;

static peer_track_t *s_peers = NULL;
static int s_peer_count = 0;

/* ── Connection state (single active connection) ───────────────── */
typedef struct {
    uint16_t conn_handle;
    uint8_t  peer_mac[6];
    char     peer_device_id[18];
    int8_t   rssi;
    char     profile_buf[BUDDY_PROFILE_MAX_BYTES];
    size_t   profile_len;
    bool     profile_sent;
    bool     active;
    bool     outgoing;   /* true = we initiated this connection */
} buddy_ble_conn_t;

/* ── Module state ──────────────────────────────────────────────── */
static QueueHandle_t s_event_queue = NULL;
static uint8_t s_own_addr_type = 0;
/* advertising runs forever while s_running */
static bool s_running = false;
static buddy_proximity_t s_last_proximity = BUDDY_PROX_UNKNOWN;
static buddy_ble_conn_t *s_conn = NULL;
static uint16_t s_chr_profile_handle = 0;
static uint16_t s_chr_profile_write_handle = 0;

/* ── BLE UUIDs ─────────────────────────────────────────────────── */
static const ble_uuid128_t g_buddy_svc_uuid =
    BLE_UUID128_INIT(BUDDY_SVC_UUID);
static const ble_uuid128_t g_buddy_chr_profile_uuid =
    BLE_UUID128_INIT(BUDDY_CHR_PROFILE_UUID);
static const ble_uuid128_t g_buddy_chr_profile_write_uuid =
    BLE_UUID128_INIT(BUDDY_CHR_PROFILE_WRITE_UUID);

/* ── Forward declarations ──────────────────────────────────────── */
static int buddy_ble_gap_event(struct ble_gap_event *event, void *arg);
static int buddy_ble_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg);
static void buddy_ble_start_adv(void);
static void buddy_ble_start_scan(void);
static void gatt_start_read(uint16_t conn_handle);

/* ── Peer tracking helpers ─────────────────────────────────────── */
static peer_track_t *peer_find_by_mac(const uint8_t *mac)
{
    for (int i = 0; i < s_peer_count; i++) {
        if (memcmp(s_peers[i].mac, mac, 6) == 0) return &s_peers[i];
    }
    return NULL;
}

static peer_track_t *peer_find_or_add(const uint8_t *mac)
{
    peer_track_t *p = peer_find_by_mac(mac);
    if (p) return p;

    if (s_peer_count < PEER_TRACK_MAX) {
        p = &s_peers[s_peer_count++];
    } else {
        /* Evict oldest */
        int oldest = 0;
        for (int i = 1; i < PEER_TRACK_MAX; i++) {
            if (s_peers[i].last_ad_ms < s_peers[oldest].last_ad_ms) oldest = i;
        }
        p = &s_peers[oldest];
    }

    memset(p, 0, sizeof(*p));
    memcpy(p->mac, mac, 6);
    return p;
}

static bool peer_should_connect(const uint8_t *mac)
{
    peer_track_t *p = peer_find_by_mac(mac);
    if (!p) return true;

    if (p->blocked) return false;

    int64_t now = esp_timer_get_time() / 1000LL;

    /* 30-min cooldown after last connection */
    if (p->last_conn_ms > 0 && (now - p->last_conn_ms) < (30 * 60 * 1000LL)) {
        return false;
    }

    /* 2-second dedup */
    if (p->last_ad_ms > 0 && (now - p->last_ad_ms) < 2000) {
        return false;
    }

    return true;
}

/* ── Advertising setup ─────────────────────────────────────────── */
static void buddy_ble_start_adv(void)
{
    if (buddy_privacy_get() == BUDDY_MODE_PRIVATE) {
        ESP_LOGI(TAG, "Privacy mode — advertising suppressed");
        return;
    }

    /* Get own device ID as short MAC */
    const buddy_identity_t *id = buddy_identity_get();
    uint8_t dev_id_bytes[6] = {0};
    sscanf(id->device_id, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
           &dev_id_bytes[0], &dev_id_bytes[1], &dev_id_bytes[2],
           &dev_id_bytes[3], &dev_id_bytes[4], &dev_id_bytes[5]);

    buddy_profile_t *profile = heap_caps_calloc(1, sizeof(*profile), MALLOC_CAP_SPIRAM);
    uint8_t profile_hash[8] = {0};
    if (profile) {
        if (buddy_profile_get(profile) == ESP_OK) {
            memcpy(profile_hash, profile->profile_hash, 8);
        }
        heap_caps_free(profile);
    }

    /* Build manufacturer data: company_id(2) + version(1) + device_id(6) + hash(8) + flags(1) */
    uint8_t mfg_buf[18];
    mfg_buf[0] = BUDDY_MFG_COMPANY_ID & 0xFF;
    mfg_buf[1] = (BUDDY_MFG_COMPANY_ID >> 8) & 0xFF;
    mfg_buf[2] = BUDDY_PROTO_VERSION;
    memcpy(&mfg_buf[3], dev_id_bytes, 6);
    memcpy(&mfg_buf[9], profile_hash, 8);
    mfg_buf[17] = 0x01;  /* flags: accepting handshakes */

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.mfg_data = mfg_buf;
    fields.mfg_data_len = sizeof(mfg_buf);

    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(900),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(1100),
        .channel_map = 0x07,
    };

    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &adv_params, buddy_ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising started (interval 900-1100ms)");
    }
}

/* ── Scanning ──────────────────────────────────────────────────── */
static void buddy_ble_start_scan(void)
{
    struct ble_gap_disc_params scan_params = {
        .itvl = BLE_GAP_SCAN_ITVL_MS(BUDDY_BLE_SCAN_INTERVAL_MS),
        .window = BLE_GAP_SCAN_WIN_MS(BUDDY_BLE_SCAN_WINDOW_MS),
        .filter_policy = 0,
        .limited = 0,
        .passive = 0,          /* active scanning */
        .filter_duplicates = 1,
    };

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER,
                          &scan_params, buddy_ble_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    } else if (rc == 0) {
        ESP_LOGI(TAG, "Scanning started (interval=%dms window=%dms)",
                 BUDDY_BLE_SCAN_INTERVAL_MS, BUDDY_BLE_SCAN_WINDOW_MS);
    }
}

/* ── Profile helpers ───────────────────────────────────────────── */
static int serialize_profile(char *buf, size_t size)
{
    const buddy_identity_t *id = buddy_identity_get();
    buddy_profile_t *profile = heap_caps_calloc(1, sizeof(*profile), MALLOC_CAP_SPIRAM);
    if (!profile) return -1;
    if (buddy_profile_get(profile) != ESP_OK) {
        heap_caps_free(profile);
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "dn", profile->display_name);
    cJSON_AddStringToObject(root, "bi", profile->bio);
    cJSON_AddStringToObject(root, "tg", profile->tags);
    cJSON_AddStringToObject(root, "vb", profile->vibe);
    cJSON_AddStringToObject(root, "ot", profile->open_to);
    cJSON_AddStringToObject(root, "cp", profile->contact_phone);
    cJSON_AddStringToObject(root, "ce", profile->contact_email);
    cJSON_AddStringToObject(root, "did", id->device_id);
    heap_caps_free(profile);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return -1;

    int len = strlen(json);
    if ((size_t)len >= size) len = size - 1;
    memcpy(buf, json, len);
    buf[len] = '\0';
    free(json);
    return len;
}

static int parse_peer_profile(const char *json, buddy_profile_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;

    memset(out, 0, sizeof(*out));
    cJSON *dn = cJSON_GetObjectItem(root, "dn");
    cJSON *bi = cJSON_GetObjectItem(root, "bi");
    cJSON *tg = cJSON_GetObjectItem(root, "tg");
    cJSON *vb = cJSON_GetObjectItem(root, "vb");
    cJSON *ot = cJSON_GetObjectItem(root, "ot");
    cJSON *cp = cJSON_GetObjectItem(root, "cp");
    cJSON *ce = cJSON_GetObjectItem(root, "ce");

    if (dn && cJSON_IsString(dn))
        snprintf(out->display_name, sizeof(out->display_name), "%s", dn->valuestring);
    if (bi && cJSON_IsString(bi))
        snprintf(out->bio, sizeof(out->bio), "%s", bi->valuestring);
    if (tg && cJSON_IsString(tg))
        snprintf(out->tags, sizeof(out->tags), "%s", tg->valuestring);
    if (vb && cJSON_IsString(vb))
        snprintf(out->vibe, sizeof(out->vibe), "%s", vb->valuestring);
    if (ot && cJSON_IsString(ot))
        snprintf(out->open_to, sizeof(out->open_to), "%s", ot->valuestring);
    if (cp && cJSON_IsString(cp))
        snprintf(out->contact_phone, sizeof(out->contact_phone), "%s", cp->valuestring);
    if (ce && cJSON_IsString(ce))
        snprintf(out->contact_email, sizeof(out->contact_email), "%s", ce->valuestring);

    cJSON_Delete(root);
    return 0;
}

/* ── Event posting ─────────────────────────────────────────────── */
static void post_profile_event(const uint8_t *peer_mac, const char *device_id,
                               int8_t rssi, const char *profile_json)
{
    buddy_event_t evt = {0};
    evt.type = BUDDY_EVT_PROFILE_READY;
    evt.peer_profile = heap_caps_calloc(1, sizeof(buddy_profile_t), MALLOC_CAP_SPIRAM);
    if (!evt.peer_profile) return;

    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             peer_mac[0], peer_mac[1], peer_mac[2],
             peer_mac[3], peer_mac[4], peer_mac[5]);
    strncpy(evt.peer_device_id, device_id[0] ? device_id : mac_str,
            sizeof(evt.peer_device_id) - 1);

    memcpy(evt.peer_mac, peer_mac, 6);
    evt.rssi = rssi;
    evt.proximity = buddy_proximity_classify();
    evt.peer_profile_valid = false;

    if (profile_json) {
        if (parse_peer_profile(profile_json, evt.peer_profile) == 0) {
            evt.peer_profile_valid = true;
        }
    }

    if (xQueueSend(s_event_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, dropping profile from %s", evt.peer_device_id);
        heap_caps_free(evt.peer_profile);
    }
}

/* ── GATT client: discover and exchange profiles ────────────────── */
static int gatt_profile_write_cb(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 struct ble_gatt_attr *attr, void *arg)
{
    if (error && error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Profile write error: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    ESP_LOGI(TAG, "Profile write complete");
    s_conn->profile_sent = true;

    /* Write done — now start the read (serialized to avoid proc limit) */
    gatt_start_read(conn_handle);

    return 0;
}

static int gatt_profile_read_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr, void *arg)
{
    if (error && error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Profile read error: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    /* attr == NULL means end-of-data (for read_long) or completion.
     * For ble_gattc_read (non-long), the data comes in attr->om directly. */
    if (!attr) {
        /* End marker — trigger disconnect if both operations done */
        if (s_conn->profile_len == 0) {
            ESP_LOGW(TAG, "Profile read returned empty, disconnecting");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else if (s_conn->profile_sent) {
            ESP_LOGI(TAG, "Profile exchange complete, disconnecting");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    }

    /* Accumulate read data */
    if (attr->om) {
        int copy_len = OS_MBUF_PKTLEN(attr->om);
        if (copy_len > 0 && s_conn->profile_len + copy_len < sizeof(s_conn->profile_buf)) {
            os_mbuf_copydata(attr->om, 0, copy_len,
                             s_conn->profile_buf + s_conn->profile_len);
            s_conn->profile_len += copy_len;
        }
    }
    return 0;
}

static int gatt_chr_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg);

static int gatt_svc_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *service, void *arg);

static void gatt_start_exchange(uint16_t conn_handle)
{
    char *own_profile = heap_caps_calloc(1, BUDDY_PROFILE_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!own_profile) {
        ESP_LOGE(TAG, "Failed to allocate own profile buffer");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }
    int own_len = serialize_profile(own_profile, BUDDY_PROFILE_MAX_BYTES);
    if (own_len < 0) {
        ESP_LOGE(TAG, "Failed to serialize own profile");
        heap_caps_free(own_profile);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    if (s_chr_profile_write_handle) {
        int rc = ble_gattc_write_flat(conn_handle,
            s_chr_profile_write_handle,
            own_profile, own_len, gatt_profile_write_cb, NULL);
        heap_caps_free(own_profile);
        if (rc != 0) {
            ESP_LOGW(TAG, "Profile write failed: %d", rc);
        }
    } else {
        heap_caps_free(own_profile);
        ESP_LOGW(TAG, "Profile write handle not found, skipping write");
        s_conn->profile_sent = true;
        gatt_start_read(conn_handle);
    }
}

static void gatt_start_read(uint16_t conn_handle)
{
    if (s_chr_profile_handle) {
        int rc = ble_gattc_read_long(conn_handle,
            s_chr_profile_handle, 0,
            gatt_profile_read_cb, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "ble_gattc_read_long failed: %d", rc);
        }
    } else {
        ESP_LOGW(TAG, "Profile read handle not found, disconnecting");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static int gatt_svc_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *service, void *arg)
{
    /* NimBLE delivers final callback with non-NULL error even on success;
     * BLE_HS_EDONE (14) means discovery complete, not an error. */
    if (error && error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Service discovery error: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    if (!service) {
        /* Discovery complete. Don't check for characteristics here —
         * characteristic discovery runs asynchronously after this. */
        return 0;
    }

    /* Log every service found for debugging */
    ESP_LOGI(TAG, "Svc found: handle=0x%04x..0x%04x type=%d",
             service->start_handle, service->end_handle,
             (int)service->uuid.u.type);

    /* Match our buddy service UUID */
    if (ble_uuid_cmp(&service->uuid.u, &g_buddy_svc_uuid.u) != 0) {
        return 0;  /* not our service, skip */
    }

    /* Found buddy service — discover characteristics */
    ESP_LOGI(TAG, "Buddy service matched (handle=0x%04x..0x%04x), discovering characteristics...",
             service->start_handle, service->end_handle);

    s_chr_profile_handle = 0;
    s_chr_profile_write_handle = 0;

    int rc = ble_gattc_disc_all_chrs(conn_handle,
        service->start_handle, service->end_handle,
        gatt_chr_disc_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "Characteristic discovery failed: %d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int gatt_chr_disc_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg)
{
    if (error && error->status != 0 && error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Characteristic discovery error: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    if (!chr) {
        /* Discovery complete — verify we found the characteristics */
        if (s_chr_profile_handle == 0 && s_chr_profile_write_handle == 0) {
            ESP_LOGW(TAG, "Buddy characteristics not found, disconnecting");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        /* Start profile exchange */
        ESP_LOGI(TAG, "Characteristic discovery complete (read=0x%04x write=0x%04x), exchanging profiles...",
                 s_chr_profile_handle, s_chr_profile_write_handle);
        gatt_start_exchange(conn_handle);
        return 0;
    }

    if (ble_uuid_cmp(&chr->uuid.u, &g_buddy_chr_profile_uuid.u) == 0) {
        s_chr_profile_handle = chr->val_handle;
        ESP_LOGI(TAG, "Found profile read characteristic: handle=0x%04x", s_chr_profile_handle);
    } else if (ble_uuid_cmp(&chr->uuid.u, &g_buddy_chr_profile_write_uuid.u) == 0) {
        s_chr_profile_write_handle = chr->val_handle;
        ESP_LOGI(TAG, "Found profile write characteristic: handle=0x%04x", s_chr_profile_write_handle);
    }
    return 0;
}

/* ── NimBLE GAP event handler ──────────────────────────────────── */
static int buddy_ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {

    case BLE_GAP_EVENT_DISC: {
        struct ble_gap_disc_desc *d = &event->disc;
        int8_t rssi = d->rssi;

        /* Extract manufacturer data */
        uint8_t dev_id[6] = {0};
        uint8_t prof_hash[8] = {0};
        uint8_t mfg_flags = 0;
        bool is_buddy = false;

        /* Parse advertising data fields */
        const uint8_t *ad = d->data;
        int ad_len = d->length_data;
        int i = 0;
        while (i < ad_len - 1) {
            uint8_t field_len = ad[i];
            uint8_t field_type = ad[i + 1];
            if (field_len == 0 || i + field_len >= ad_len) break;

            if (field_type == 0xFF && field_len >= 19) {
                /* Manufacturer Specific Data */
                uint16_t company = ad[i + 2] | (ad[i + 3] << 8);
                if (company == BUDDY_MFG_COMPANY_ID) {
                    uint8_t ver = ad[i + 4];
                    if (ver == BUDDY_PROTO_VERSION) {
                        memcpy(dev_id, &ad[i + 5], 6);
                        memcpy(prof_hash, &ad[i + 11], 8);
                        mfg_flags = ad[i + 19];
                        is_buddy = true;
                    }
                }
            }
            i += field_len + 1;
        }

        if (!is_buddy) break;

        /* Skip if privacy flag set */
        if (mfg_flags & 0x04) break;

        /* Peer tracking */
        peer_track_t *p = peer_find_or_add(d->addr.val);
        p->rssi = rssi;
        int64_t now = esp_timer_get_time() / 1000LL;

        char did[18];
        snprintf(did, sizeof(did), "%02x:%02x:%02x:%02x:%02x:%02x",
                 dev_id[0], dev_id[1], dev_id[2], dev_id[3], dev_id[4], dev_id[5]);
        strncpy(p->device_id, did, sizeof(p->device_id) - 1);

        buddy_proximity_feed(rssi);
        buddy_proximity_t prox = buddy_proximity_classify();
        if (prox != s_last_proximity) {
            ESP_LOGI(TAG, "Proximity: %s (rssi=%d)",
                     buddy_proximity_str(prox), rssi);
            s_last_proximity = prox;
        }

        /* Check if we should connect (before updating last_ad_ms, so
         * peer_should_connect sees the PREVIOUS ad timestamp for dedup) */
        if (!peer_should_connect(d->addr.val)) break;
        if (buddy_privacy_get() == BUDDY_MODE_PRIVATE) break;
        if (!(mfg_flags & 0x01)) break;  /* not accepting */
        if (prox < BUDDY_PROX_NEAR) break;  /* only connect when near */
        if (s_conn->active) break;  /* already handling a connection */
        if (s_conn->conn_handle != 0) break;  /* already have an incoming connection */

        p->last_ad_ms = now;

        ESP_LOGI(TAG, "New buddy detected: %s (rssi=%d, prox=%s)",
                 did, rssi, buddy_proximity_str(prox));

        /* Initiate connection — cancel scan to free the radio */
        p->last_conn_ms = now;
        memset(s_conn, 0, sizeof(*s_conn));
        s_conn->active = true;
        s_conn->outgoing = true;
        memcpy(s_conn->peer_mac, d->addr.val, 6);
        strncpy(s_conn->peer_device_id, did, sizeof(s_conn->peer_device_id) - 1);
        s_conn->rssi = rssi;

        ble_gap_disc_cancel();
        vTaskDelay(pdMS_TO_TICKS(30));

        int rc = ble_gap_connect(s_own_addr_type, &d->addr,
                                 BLE_HS_FOREVER, NULL,
                                 buddy_ble_gap_event, NULL);
        if (rc != 0) {
            if (rc != BLE_HS_EDONE) {
                ESP_LOGW(TAG, "ble_gap_connect failed: %d", rc);
            }
            s_conn->active = false;
            s_conn->outgoing = false;
            /* Restart scanning */
            if (s_running) buddy_ble_start_scan();
        }
        break;
    }

    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "Connection failed: status=%d", event->connect.status);
            s_conn->active = false;
            s_conn->outgoing = false;
            if (s_running) buddy_ble_start_scan();
            break;
        }

        s_conn->conn_handle = event->connect.conn_handle;

        if (s_conn->outgoing) {
            /* We initiated — discover peer's GATT services */
            ESP_LOGI(TAG, "Connected (handle=%d), discovering all services...",
                     s_conn->conn_handle);
            int rc = ble_gattc_disc_all_svcs(s_conn->conn_handle,
                gatt_svc_disc_cb, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "Service discovery failed: %d", rc);
                ble_gap_terminate(s_conn->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
        } else {
            /* Incoming connection — peer will discover our services */
            ESP_LOGI(TAG, "Incoming connection (handle=%d), waiting for peer to discover...",
                     s_conn->conn_handle);
        }
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        ESP_LOGI(TAG, "Disconnect (reason=%d)", event->disconnect.reason);

        /* If we received a profile, post event */
        if (s_conn->profile_len > 0) {
            post_profile_event(s_conn->peer_mac, s_conn->peer_device_id,
                               s_conn->rssi, s_conn->profile_buf);
        }

        memset(s_conn, 0, sizeof(*s_conn));

        /* Restart scanning for next buddy */
        if (s_running) buddy_ble_start_scan();
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE: {
        /* Scan stopped (either cycle end or cancelled for connection).
         * Only restart if we're not in the middle of a connection. */
        if (s_running && !s_conn->active) {
            buddy_ble_start_scan();
        }
        break;
    }

    case BLE_GAP_EVENT_NOTIFY_RX:
    case BLE_GAP_EVENT_NOTIFY_TX:
    case BLE_GAP_EVENT_ADV_COMPLETE:
    case BLE_GAP_EVENT_SUBSCRIBE:
    case BLE_GAP_EVENT_MTU:
        break;
    }

    return 0;
}

/* ── GATT access callback (peripheral side) ────────────────────── */
static int buddy_ble_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const ble_uuid_t *uuid = ctxt->chr->uuid;

    if (ble_uuid_cmp(uuid, &g_buddy_chr_profile_uuid.u) == 0) {
        /* READ: return our profile JSON (use PSRAM to limit stack usage) */
        char *profile_json = heap_caps_calloc(1, BUDDY_PROFILE_MAX_BYTES, MALLOC_CAP_SPIRAM);
        if (!profile_json) return BLE_ATT_ERR_INSUFFICIENT_RES;
        int len = serialize_profile(profile_json, BUDDY_PROFILE_MAX_BYTES);
        if (len < 0) {
            heap_caps_free(profile_json);
            return BLE_ATT_ERR_UNLIKELY;
        }

        int rc = os_mbuf_append(ctxt->om, profile_json, len);
        heap_caps_free(profile_json);
        return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ble_uuid_cmp(uuid, &g_buddy_chr_profile_write_uuid.u) == 0) {
        /* WRITE: peer is sending their profile (use PSRAM to limit stack usage) */
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len == 0 || om_len >= BUDDY_PROFILE_MAX_BYTES) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        char *buf = heap_caps_calloc(1, BUDDY_PROFILE_MAX_BYTES, MALLOC_CAP_SPIRAM);
        if (!buf) return BLE_ATT_ERR_INSUFFICIENT_RES;
        os_mbuf_copydata(ctxt->om, 0, om_len, buf);
        buf[om_len] = '\0';

        /* Get peer MAC from connection info */
        uint8_t peer_mac[6] = {0};
        int8_t rssi = -90;
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(conn_handle, &desc) == 0) {
            memcpy(peer_mac, desc.peer_id_addr.val, 6);
        }
        /* Also use s_conn if we initiated (has RSSI from scan) */
        if (memcmp(peer_mac, s_conn->peer_mac, 6) == 0 && s_conn->rssi != 0) {
            rssi = s_conn->rssi;
        }

        /* Extract device_id from profile */
        char did[18] = "unknown";
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            cJSON *didj = cJSON_GetObjectItem(root, "did");
            if (didj && cJSON_IsString(didj)) {
                strncpy(did, didj->valuestring, sizeof(did) - 1);
            }
            cJSON_Delete(root);
        }

        post_profile_event(peer_mac, did, rssi, buf);
        heap_caps_free(buf);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ── GATT service definition ───────────────────────────────────── */
static struct ble_gatt_chr_def g_buddy_chrs[] = {
    {
        .uuid = &g_buddy_chr_profile_uuid.u,
        .access_cb = buddy_ble_gatt_access,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &g_buddy_chr_profile_write_uuid.u,
        .access_cb = buddy_ble_gatt_access,
        .flags = BLE_GATT_CHR_F_WRITE,
    },
    { 0 }
};

static const struct ble_gatt_svc_def g_buddy_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &g_buddy_svc_uuid.u,
        .characteristics = g_buddy_chrs,
    },
    { 0 }
};

/* ── NimBLE sync / reset callbacks ─────────────────────────────── */
static void buddy_ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    /* Register GATT services and force re-processing.
     * ble_gatts_start() already ran during nimble_port_init() with an
     * empty svc_defs array. We re-add now and call start again to
     * actually register them with the ATT server. */
    int num_handles = ble_gatts_count_cfg(g_buddy_svcs);
    if (num_handles < 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", num_handles);
        return;
    }
    rc = ble_gatts_add_svcs(g_buddy_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return;
    }
    rc = ble_gatts_start();
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_start (re-register) failed: %d", rc);
        return;
    }

    ble_att_set_preferred_mtu(512);

    ESP_LOGI(TAG, "NimBLE synced, GATT services registered (%d handles)",
             num_handles);

    if (s_running) {
        buddy_ble_start_adv();
        buddy_ble_start_scan();
    }
}

static void buddy_ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset (reason=%d)", reason);
}

/* ── NimBLE host task ──────────────────────────────────────────── */
static void buddy_ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ── Public API ────────────────────────────────────────────────── */
esp_err_t buddy_ble_init(void)
{
    s_peers = heap_caps_calloc(PEER_TRACK_MAX, sizeof(*s_peers), MALLOC_CAP_SPIRAM);
    s_conn  = heap_caps_calloc(1, sizeof(*s_conn), MALLOC_CAP_SPIRAM);
    if (!s_peers || !s_conn) {
        ESP_LOGE(TAG, "Failed to allocate peers/conn in PSRAM");
        return ESP_ERR_NO_MEM;
    }
    s_peer_count = 0;

    s_event_queue = xQueueCreate(4, sizeof(buddy_event_t));
    if (!s_event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_ERR_NO_MEM;
    }

    /* nimble_port_init() handles BT controller init + enable + NimBLE host init internally.
     * We must NOT call esp_bt_controller_init() ourselves — that would conflict. */
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.reset_cb = buddy_ble_on_reset;
    ble_hs_cfg.sync_cb = buddy_ble_on_sync;

    nimble_port_freertos_init(buddy_ble_host_task);

    /* Wait for NimBLE sync to complete */
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "BLE transport initialized");
    return ESP_OK;
}

esp_err_t buddy_ble_start(void)
{
    if (s_running) return ESP_OK;

    s_running = true;

    /* If already synced, start now; otherwise on_sync will start us */
    if (ble_hs_synced()) {
        buddy_ble_start_adv();
        buddy_ble_start_scan();
    } else {
        ESP_LOGI(TAG, "Waiting for NimBLE sync...");
    }

    ESP_LOGI(TAG, "BLE transport started");
    return ESP_OK;
}

esp_err_t buddy_ble_stop(void)
{
    if (!s_running) return ESP_OK;

    s_running = false;

    ble_gap_adv_stop();
    /* Scan will stop when current cycle ends, or we could cancel */
    ble_gap_disc_cancel();

    if (s_conn->active) {
        ble_gap_terminate(s_conn->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    ESP_LOGI(TAG, "BLE transport stopped");
    return ESP_OK;
}

QueueHandle_t buddy_ble_get_event_queue(void)
{
    return s_event_queue;
}
