#include "tool_web_search.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "web_search";

static char s_search_key[128] = {0};

#define SEARCH_BUF_SIZE     (16 * 1024)
#define SEARCH_RESULT_COUNT 5

/* ── Response accumulator ─────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} search_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    search_buf_t *sb = (search_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t needed = sb->len + evt->data_len;
        if (needed < sb->cap) {
            memcpy(sb->data + sb->len, evt->data, evt->data_len);
            sb->len += evt->data_len;
            sb->data[sb->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t tool_web_search_init(void)
{
    /* Start with build-time default */
    if (MIMI_SECRET_SEARCH_KEY[0] != '\0') {
        strncpy(s_search_key, MIMI_SECRET_SEARCH_KEY, sizeof(s_search_key) - 1);
    }

    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_SEARCH, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_search_key, tmp, sizeof(s_search_key) - 1);
        }
        nvs_close(nvs);
    }

    if (s_search_key[0]) {
        ESP_LOGI(TAG, "Tavily search initialized (key configured)");
    } else {
        ESP_LOGW(TAG, "No Tavily API key. Use CLI: set_search_key <KEY>");
    }
    return ESP_OK;
}

/* ── Build Tavily POST request body ───────────────────────────── */

static char *build_tavily_request(const char *query)
{
    cJSON *body = cJSON_CreateObject();
    if (!body) return NULL;

    cJSON_AddStringToObject(body, "api_key", s_search_key);
    cJSON_AddStringToObject(body, "query", query);
    cJSON_AddStringToObject(body, "search_depth", "basic");
    cJSON_AddNumberToObject(body, "max_results", SEARCH_RESULT_COUNT);
    cJSON_AddBoolToObject(body, "include_answer", false);
    cJSON_AddBoolToObject(body, "include_raw_content", false);

    char *json_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return json_str;
}

/* ── Format results as readable text (Tavily response format) ─── */

static void format_results(cJSON *root, char *output, size_t output_size)
{
    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No web results found.");
        return;
    }

    size_t off = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (idx >= SEARCH_RESULT_COUNT) break;

        cJSON *title = cJSON_GetObjectItem(item, "title");
        cJSON *url = cJSON_GetObjectItem(item, "url");
        cJSON *content = cJSON_GetObjectItem(item, "content");

        off += snprintf(output + off, output_size - off,
            "%d. %s\n   %s\n   %s\n\n",
            idx + 1,
            (title && cJSON_IsString(title)) ? title->valuestring : "(no title)",
            (url && cJSON_IsString(url)) ? url->valuestring : "",
            (content && cJSON_IsString(content)) ? content->valuestring : "");

        if (off >= output_size - 1) break;
        idx++;
    }
}

/* ── Direct HTTPS request (Tavily POST) ───────────────────────── */

static esp_err_t search_direct(const char *post_body, search_buf_t *sb)
{
    esp_http_client_config_t config = {
        .url = "https://api.tavily.com/search",
        .event_handler = http_event_handler,
        .user_data = sb,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_POST,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, post_body, strlen(post_body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (status != 200) {
        ESP_LOGE(TAG, "Tavily API returned %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Proxy HTTPS request (Tavily POST) ────────────────────────── */

static esp_err_t search_via_proxy(const char *post_body, search_buf_t *sb)
{
    proxy_conn_t *conn = proxy_conn_open("api.tavily.com", 443, 15000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    int body_len = strlen(post_body);
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "POST /search HTTP/1.1\r\n"
        "Host: api.tavily.com\r\n"
        "Content-Type: application/json\r\n"
        "Accept: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        body_len);

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    if (proxy_conn_write(conn, post_body, body_len) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    /* Read full response */
    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
        if (n <= 0) break;
        size_t copy = (total + n < sb->cap - 1) ? (size_t)n : sb->cap - 1 - total;
        if (copy > 0) {
            memcpy(sb->data + total, tmp, copy);
            total += copy;
        }
    }
    sb->data[total] = '\0';
    sb->len = total;
    proxy_conn_close(conn);

    /* Check status */
    int status = 0;
    if (total > 5 && strncmp(sb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(sb->data, ' ');
        if (sp) status = atoi(sp + 1);
    }

    /* Strip headers */
    char *body = strstr(sb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = total - (body - sb->data);
        memmove(sb->data, body, blen);
        sb->len = blen;
        sb->data[sb->len] = '\0';
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Tavily API returned %d via proxy", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_web_search_execute(const char *input_json, char *output, size_t output_size)
{
    if (s_search_key[0] == '\0') {
        snprintf(output, output_size, "Error: No Tavily API key configured. Set MIMI_SECRET_SEARCH_KEY in mimi_secrets.h");
        return ESP_ERR_INVALID_STATE;
    }

    /* Parse input to get query */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *query = cJSON_GetObjectItem(input, "query");
    if (!query || !cJSON_IsString(query) || query->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'query' field");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Searching: %s", query->valuestring);

    /* Build Tavily POST request body */
    char *post_body = build_tavily_request(query->valuestring);
    cJSON_Delete(input);

    if (!post_body) {
        snprintf(output, output_size, "Error: Failed to build request");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate response buffer from PSRAM */
    search_buf_t sb = {0};
    sb.data = heap_caps_calloc(1, SEARCH_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!sb.data) {
        free(post_body);
        snprintf(output, output_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }
    sb.cap = SEARCH_BUF_SIZE;

    /* Make HTTP request */
    esp_err_t err;
    if (http_proxy_is_enabled()) {
        err = search_via_proxy(post_body, &sb);
    } else {
        err = search_direct(post_body, &sb);
    }

    free(post_body);

    if (err != ESP_OK) {
        free(sb.data);
        snprintf(output, output_size, "Error: Search request failed");
        return err;
    }

    /* Parse and format results */
    cJSON *root = cJSON_Parse(sb.data);
    free(sb.data);

    if (!root) {
        snprintf(output, output_size, "Error: Failed to parse search results");
        return ESP_FAIL;
    }

    format_results(root, output, output_size);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Search complete, %d bytes result", (int)strlen(output));
    return ESP_OK;
}

esp_err_t tool_web_search_set_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_SEARCH, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_search_key, api_key, sizeof(s_search_key) - 1);
    ESP_LOGI(TAG, "Tavily API key saved");
    return ESP_OK;
}
