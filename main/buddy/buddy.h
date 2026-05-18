#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── BLE transport ──────────────────────────────────────────────── */
#define BUDDY_BLE_ADV_PERIOD_MS    1000
#define BUDDY_BLE_SCAN_INTERVAL_MS  1000
#define BUDDY_BLE_SCAN_WINDOW_MS    800

/* ── Beacon timing ───────────────────────────────────────────── */
#define BUDDY_BEACON_PERIOD_MS      250
#define BUDDY_BEACON_JITTER_MS      50
#define BUDDY_PROFILE_HASH_LEN      8

/* ── Proximity thresholds (RSSI dBm) ──────────────────────────── */
#define BUDDY_PROXIMITY_NEAR        (-55)
#define BUDDY_PROXIMITY_MID         (-70)
#define BUDDY_PROXIMITY_FAR         (-85)
#define BUDDY_RSSI_SAMPLES          5

/* ── Discovery limits ────────────────────────────────────────── */
#define BUDDY_BEACON_DEDUP_MS       2000
#define BUDDY_REHANDSHAKE_COOLDOWN_S (30 * 60)

/* ── Profile limits ──────────────────────────────────────────── */
#define BUDDY_PROFILE_MAX_BYTES     1536
#define BUDDY_DEVICE_ID_LEN         18
#define BUDDY_DISPLAY_NAME_LEN      32
#define BUDDY_BIO_LEN               1024
#define BUDDY_TAGS_LEN              128
#define BUDDY_CONTACT_PHONE_LEN     20
#define BUDDY_CONTACT_EMAIL_LEN     64
#define BUDDY_ICEBREAKER_LEN        1024
#define BUDDY_MAX_CONTACTS          500

/* ── Proximity classes ───────────────────────────────────────── */
typedef enum {
    BUDDY_PROX_UNKNOWN = 0,
    BUDDY_PROX_FAR,
    BUDDY_PROX_MID,
    BUDDY_PROX_NEAR,
} buddy_proximity_t;

/* ── Contact status ──────────────────────────────────────────── */
typedef enum {
    BUDDY_CONTACT_NEW = 0,
    BUDDY_CONTACT_KNOWN,
    BUDDY_CONTACT_RECENT,
} buddy_contact_status_t;

/* ── Privacy mode ────────────────────────────────────────────── */
typedef enum {
    BUDDY_MODE_PUBLIC = 0,
    BUDDY_MODE_PRIVATE,
} buddy_privacy_mode_t;

/* ── User profile ────────────────────────────────────────────── */
typedef struct {
    uint8_t  version;
    char     display_name[BUDDY_DISPLAY_NAME_LEN];
    char     bio[BUDDY_BIO_LEN];
    char     tags[BUDDY_TAGS_LEN];
    char     vibe[16];
    char     open_to[64];
    char     contact_phone[BUDDY_CONTACT_PHONE_LEN];
    char     contact_email[BUDDY_CONTACT_EMAIL_LEN];
    uint8_t  profile_hash[BUDDY_PROFILE_HASH_LEN];
} buddy_profile_t;

/* ── Device identity (generated once at first boot) ──────────── */
typedef struct {
    char     device_id[BUDDY_DEVICE_ID_LEN];   /* MAC string */
    uint8_t  ed25519_public[32];
    uint8_t  ed25519_private[32];              /* stored in NVS, never transmitted */
} buddy_identity_t;

/* ── Contact record (stored on-device only) ──────────────────── */
typedef struct {
    char     peer_id[BUDDY_DEVICE_ID_LEN];
    char     display_name[BUDDY_DISPLAY_NAME_LEN];
    char     tags[BUDDY_TAGS_LEN];
    char     bio[BUDDY_BIO_LEN];
    char     contact_phone[BUDDY_CONTACT_PHONE_LEN];
    char     contact_email[BUDDY_CONTACT_EMAIL_LEN];
    char     feishu_open_id[64];
    int64_t  last_met_unix;
    uint16_t meeting_count;
    float    match_score;
    char     icebreaker[BUDDY_ICEBREAKER_LEN];
    char     shared_interests[128];
    bool     cloud_synced;
} buddy_contact_record_t;

/* ── Internal buddy event ────────────────────────────────────── */
typedef enum {
    BUDDY_EVT_PEER_DISCOVERED = 0,
    BUDDY_EVT_HANDSHAKE_COMPLETE,
    BUDDY_EVT_HANDSHAKE_FAILED,
    BUDDY_EVT_PROFILE_READY,
    BUDDY_EVT_CLOUD_MATCH_DONE,
    BUDDY_EVT_FEISHU_SENT,
    BUDDY_EVT_FEISHU_FAIL,
} buddy_event_type_t;

typedef struct {
    buddy_event_type_t type;
    uint8_t  peer_mac[6];
    char     peer_device_id[BUDDY_DEVICE_ID_LEN];
    int8_t   rssi;
    buddy_proximity_t proximity;
    buddy_profile_t   *peer_profile;   /* allocated from PSRAM, consumer frees */
    bool     peer_profile_valid;
} buddy_event_t;

/* ── LED feedback ────────────────────────────────────────────── */
typedef enum {
    BUDDY_LED_PATTERN_OFF = 0,
    BUDDY_LED_PATTERN_BLUE_SLOW,
    BUDDY_LED_PATTERN_AMBER,
    BUDDY_LED_PATTERN_GREEN_FAST,
    BUDDY_LED_PATTERN_AMBER_BRIEF,    /* missed - no WiFi */
} buddy_led_pattern_t;

/* ══════════════════════════════════════════════════════════════
   Public API
   ══════════════════════════════════════════════════════════════ */

/**
 * Initialize the full buddy subsystem:
 *   - Load/generate device identity (Ed25519 keypair)
 *   - Load user profile from NVS
 *   - Mount contact store from SPIFFS
 *   - Initialize BLE transport
 *   - Start beacon TX and RX tasks
 *   - Start contact processing task
 *   - Init LED
 */
esp_err_t buddy_init(void);

/**
 * Start buddy discovery (BLE advertising + scanning).
 */
esp_err_t buddy_start(void);

/**
 * Stop buddy discovery (BLE advertising + scanning + connections).
 */
esp_err_t buddy_stop(void);

/**
 * Get the event queue for contact processing.
 */
QueueHandle_t buddy_ble_get_event_queue(void);

/**
 * Get the current user profile. Caller provides buffer.
 */
esp_err_t buddy_profile_get(buddy_profile_t *out);

/**
 * Save user profile to NVS and recompute beacon hash.
 */
esp_err_t buddy_profile_set(const buddy_profile_t *profile);

/**
 * Get device identity.
 */
const buddy_identity_t *buddy_identity_get(void);

/**
 * Set privacy mode (PUBLIC = broadcasting, PRIVATE = silent).
 */
esp_err_t buddy_privacy_set(buddy_privacy_mode_t mode);

/**
 * Get current privacy mode.
 */
buddy_privacy_mode_t buddy_privacy_get(void);

/**
 * Look up a contact record by peer device_id.
 * Returns ESP_OK and fills *out, or ESP_ERR_NOT_FOUND.
 */
esp_err_t buddy_contacts_get(const char *peer_id, buddy_contact_record_t *out);

/**
 * List contacts. Fills buf with up to max records. *count is updated.
 */
esp_err_t buddy_contacts_list(buddy_contact_record_t *buf, size_t max, size_t *count);

/**
 * Trigger a buddy match flow from external source (e.g. WiFi onboarding debug).
 * Both profiles are consumed — caller frees nothing.
 */
esp_err_t buddy_match_trigger(const buddy_profile_t *self,
                              const buddy_profile_t *peer,
                              buddy_contact_status_t status,
                              buddy_proximity_t proximity);

/**
 * Set LED pattern. Non-blocking, async-safe.
 */
esp_err_t buddy_led_set(buddy_led_pattern_t pattern);

/**
 * cleanup all contacts and reset the contact store (for debug)
 * tool_exec write_file {"path":"/spiffs/contacts.json","content":"[]"}
 */