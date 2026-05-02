#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── ESP-NOW channel ─────────────────────────────────────────── */
#define BUDDY_ESPNOW_CHANNEL        6
#define BUDDY_ESPNOW_PMK            "buddy_pmk_16byte"  /* must be exactly 16 bytes */
#define BUDDY_SESSION_PSK           "buddy_session_v1!"  /* 16 bytes for AES-GCM session key */

/* ── Beacon timing ───────────────────────────────────────────── */
#define BUDDY_BEACON_PERIOD_MS      250
#define BUDDY_BEACON_JITTER_MS      50
#define BUDDY_PROFILE_HASH_LEN      8

/* ── Proximity thresholds (RSSI dBm) ──────────────────────────── */
#define BUDDY_PROXIMITY_NEAR        (-55)
#define BUDDY_PROXIMITY_MID         (-70)
#define BUDDY_PROXIMITY_FAR         (-85)
#define BUDDY_RSSI_SAMPLES          5

/* ── Handshake limits ────────────────────────────────────────── */
#define BUDDY_MAX_CONCURRENT_HS     3
#define BUDDY_HS_RETRY_MAX          3
#define BUDDY_HS_TIMEOUT_MS         5000
#define BUDDY_BEACON_DEDUP_MS       2000
#define BUDDY_REHANDSHAKE_COOLDOWN_S (30 * 60)

/* ── Profile / payload limits ────────────────────────────────── */
#define BUDDY_PROFILE_MAX_BYTES     720
#define BUDDY_ESPNOW_MAX_PAYLOAD    250
#define BUDDY_MAX_FRAGMENTS         3
#define BUDDY_DEVICE_ID_LEN         18
#define BUDDY_DISPLAY_NAME_LEN      32
#define BUDDY_BIO_LEN               512
#define BUDDY_TAGS_LEN              128
#define BUDDY_CONTACT_PHONE_LEN     20
#define BUDDY_CONTACT_EMAIL_LEN     64
#define BUDDY_MAX_CONTACTS          500

/* ── Frame types ─────────────────────────────────────────────── */
#define BUDDY_FRAME_TYPE_BEACON     0x01
#define BUDDY_FRAME_TYPE_HS_REQ     0x02
#define BUDDY_FRAME_TYPE_HS_RESP    0x03
#define BUDDY_FRAME_TYPE_PROFILE    0x04

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
    char     icebreaker[256];
    char     shared_interests[128];
    bool     cloud_synced;
} buddy_contact_record_t;

/* ── ESP-NOW frame types (wire format) ────────────────────────── */

/* Beacon: 32 bytes broadcast */
typedef struct __attribute__((packed)) {
    uint8_t  version;                          /* protocol version */
    uint8_t  type;                             /* BUDDY_FRAME_TYPE_BEACON */
    uint8_t  device_id[6];                     /* short MAC */
    uint8_t  profile_hash[8];                  /* first 8 bytes of SHA-256(profile) */
    int8_t   rssi_cal;                         /* factory TX power offset */
    uint8_t  nonce[4];                         /* random, for freshness */
    uint8_t  flags;                            /* bit0=accepting, bit1=battery_low, bit2=private */
    uint8_t  reserved[3];
} buddy_beacon_frame_t;

/* Handshake request: 27 bytes unicast */
typedef struct __attribute__((packed)) {
    uint8_t  type;                             /* BUDDY_FRAME_TYPE_HS_REQ */
    uint8_t  device_id[6];
    uint32_t timestamp;
    uint8_t  nonce[16];                        /* session key derivation */
} buddy_hs_req_frame_t;

/* Handshake response: 23 bytes unicast */
typedef struct __attribute__((packed)) {
    uint8_t  type;                             /* BUDDY_FRAME_TYPE_HS_RESP */
    uint8_t  device_id[6];
    uint8_t  req_nonce[16];                    /* copied from HS_REQ */
} buddy_hs_resp_frame_t;

/* Profile payload fragment */
typedef struct __attribute__((packed)) {
    uint8_t  type;                             /* BUDDY_FRAME_TYPE_PROFILE */
    uint8_t  seq;                              /* sequence number */
    uint8_t  total;                            /* total fragments */
    uint8_t  encrypted_payload[BUDDY_ESPNOW_MAX_PAYLOAD - 3 - 16];
    uint8_t  auth_tag[16];                     /* AES-128-GCM tag */
} buddy_profile_frag_frame_t;

/* ── Rendezvous state (per-peer during handshake) ────────────── */
typedef struct {
    uint8_t  peer_mac[6];
    uint8_t  peer_device_id[BUDDY_DEVICE_ID_LEN];
    int8_t   rssi;
    uint8_t  nonce[16];                        /* initiator's nonce (session key material) */
    uint32_t timestamp_start;
    uint8_t  aes_key[16];
    uint8_t  aes_iv[12];
    uint8_t  reassembly_buf[BUDDY_PROFILE_MAX_BYTES];
    size_t   reassembly_len;
    uint8_t  pending_frags;
    uint8_t  retry_count;
    bool     active;
    bool     profile_complete;
    bool     profile_sent;                     /* avoid double-send in simultaneous handshake */
} buddy_handshake_t;

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
    buddy_profile_t   peer_profile;
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
 *   - Initialize ESP-NOW
 *   - Start beacon TX and RX tasks
 *   - Start contact processing task
 *   - Init LED
 */
esp_err_t buddy_init(void);

/**
 * Start buddy discovery (beacon broadcast + receive).
 * Call after WiFi is connected (for coexistence setup).
 */
esp_err_t buddy_start(void);

/**
 * Stop buddy discovery and teardown ESP-NOW.
 */
esp_err_t buddy_stop(void);

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