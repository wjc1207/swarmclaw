#include "tool_arxiv.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

static const char *TAG = "arxiv";

#define ARXIV_BUF_SIZE        (32 * 1024)
#define ARXIV_DEFAULT_COUNT   5
#define ARXIV_MAX_COUNT       20
#define ARXIV_CACHE_KEY_LEN   256
#define ARXIV_CACHE_VAL_SIZE  (8 * 1024)

/* ── Thread safety ────────────────────────────────────────────── */

static SemaphoreHandle_t s_mutex = NULL;

/* ── Single-entry query cache ─────────────────────────────────── */

static char  s_cache_key[ARXIV_CACHE_KEY_LEN] = {0};
static char *s_cache_val = NULL;   /* allocated from PSRAM on init */

/* ── Response accumulator ─────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} arxiv_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    arxiv_buf_t *ab = (arxiv_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t needed = ab->len + evt->data_len;
        if (needed < ab->cap - 1) {
            memcpy(ab->data + ab->len, evt->data, evt->data_len);
            ab->len += evt->data_len;
            ab->data[ab->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t tool_arxiv_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_cache_val = heap_caps_calloc(1, ARXIV_CACHE_VAL_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_cache_val) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
        ESP_LOGE(TAG, "Cache buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "arXiv search tool initialized");
    return ESP_OK;
}

/* ── URL-encode a query string ────────────────────────────────── */

static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    for (; *src && pos < dst_size - 3; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
    return pos;
}

/* ── Bounded substring search ─────────────────────────────────── */

static const char *mem_find(const char *hay, size_t hlen, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) return hay;
    if (nlen > hlen) return NULL;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) return hay + i;
    }
    return NULL;
}

/* ── Extract XML tag content within a bounded region ─────────── */

static bool xml_extract(const char *region, size_t rlen,
                        const char *open_tag, const char *close_tag,
                        char *out, size_t out_size)
{
    const char *p = mem_find(region, rlen, open_tag);
    if (!p) return false;
    p += strlen(open_tag);

    size_t left = rlen - (size_t)(p - region);
    const char *q = mem_find(p, left, close_tag);
    if (!q) return false;

    size_t len = (size_t)(q - p);

    /* Trim leading whitespace */
    while (len > 0 && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) {
        p++;
        len--;
    }
    /* Trim trailing whitespace */
    while (len > 0 &&
           (p[len - 1] == ' ' || p[len - 1] == '\n' ||
            p[len - 1] == '\r' || p[len - 1] == '\t')) {
        len--;
    }

    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

/* ── Collect all author <name> values within an entry ─────────── */

static void extract_authors(const char *entry, size_t entry_len,
                             char *out, size_t out_size)
{
    size_t search_off = 0;
    size_t out_off = 0;
    int count = 0;

    while (search_off < entry_len) {
        const char *p = mem_find(entry + search_off,
                                 entry_len - search_off, "<name>");
        if (!p) break;
        p += strlen("<name>");

        size_t left = entry_len - (size_t)(p - entry);
        const char *q = mem_find(p, left, "</name>");
        if (!q) break;

        size_t nlen = (size_t)(q - p);
        /* Trim whitespace */
        while (nlen > 0 && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) {
            p++;
            nlen--;
        }
        while (nlen > 0 &&
               (p[nlen - 1] == ' ' || p[nlen - 1] == '\n' ||
                p[nlen - 1] == '\r' || p[nlen - 1] == '\t')) {
            nlen--;
        }

        if (nlen > 0 && out_off < out_size - 1) {
            if (count > 0 && out_off + 2 < out_size) {
                out[out_off++] = ',';
                out[out_off++] = ' ';
            }
            size_t copy = nlen;
            if (copy >= out_size - out_off - 1) copy = out_size - out_off - 1;
            memcpy(out + out_off, p, copy);
            out_off += copy;
            count++;
        }

        search_off = (size_t)(q + strlen("</name>") - entry);
    }
    out[out_off] = '\0';
}

/* ── Parse arXiv Atom feed and format results ─────────────────── */

static void format_results(const char *xml, char *output, size_t output_size)
{
    size_t xml_len = strlen(xml);
    size_t xml_off = 0;   /* position in XML input  */
    size_t out_off = 0;   /* position in output buf */
    int idx = 0;

    while (xml_off < xml_len) {
        /* Find next <entry> block */
        const char *entry_start = mem_find(xml + xml_off, xml_len - xml_off,
                                           "<entry>");
        if (!entry_start) break;
        entry_start += strlen("<entry>");

        size_t remaining = xml_len - (size_t)(entry_start - xml);
        const char *entry_end = mem_find(entry_start, remaining, "</entry>");
        if (!entry_end) break;

        size_t entry_len = (size_t)(entry_end - entry_start);

        /* Extract fields */
        char title[256]   = {0};
        char link[256]    = {0};
        char summary[512] = {0};
        char authors[256] = {0};

        if (!xml_extract(entry_start, entry_len, "<title>", "</title>",
                         title, sizeof(title))) {
            strcpy(title, "(no title)");
        }
        xml_extract(entry_start, entry_len, "<id>", "</id>",
                    link, sizeof(link));
        xml_extract(entry_start, entry_len, "<summary>", "</summary>",
                    summary, sizeof(summary));
        extract_authors(entry_start, entry_len, authors, sizeof(authors));

        /* Truncate long summaries */
        const size_t MAX_SUMMARY = 400;
        if (strlen(summary) > MAX_SUMMARY) {
            summary[MAX_SUMMARY] = '\0';
            /* Back up to last space to avoid cutting mid-word */
            char *last_space = strrchr(summary, ' ');
            if (last_space) *last_space = '\0';
            strncat(summary, "...", sizeof(summary) - strlen(summary) - 1);
        }

        int written = snprintf(output + out_off, output_size - out_off,
            "%d. %s\n"
            "   Authors : %s\n"
            "   Link    : %s\n"
            "   Summary : %s\n\n",
            idx + 1,
            title,
            authors[0] ? authors : "(unknown)",
            link[0]    ? link    : "(no link)",
            summary[0] ? summary : "(no summary)");

        if (written <= 0 || (size_t)written >= output_size - out_off) break;
        out_off += written;
        idx++;

        /* Advance past this entry in the XML */
        xml_off = (size_t)(entry_end + strlen("</entry>") - xml);
    }

    if (idx == 0) {
        snprintf(output, output_size, "No arXiv results found for the given keywords.");
    }
}

/* ── Direct HTTPS request ─────────────────────────────────────── */

static esp_err_t arxiv_direct(const char *url, arxiv_buf_t *ab)
{
    esp_http_client_config_t config = {
        .url            = url,
        .event_handler  = http_event_handler,
        .user_data      = ab,
        .timeout_ms     = 15000,
        .buffer_size    = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Accept", "application/atom+xml");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (status != 200) {
        ESP_LOGE(TAG, "arXiv API returned HTTP %d", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Proxy HTTPS request ──────────────────────────────────────── */

static esp_err_t arxiv_via_proxy(const char *path, arxiv_buf_t *ab)
{
    proxy_conn_t *conn = proxy_conn_open("export.arxiv.org", 443, 15000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "GET %s HTTP/1.1\r\n"
        "Host: export.arxiv.org\r\n"
        "Accept: application/atom+xml\r\n"
        "Connection: close\r\n\r\n",
        path);
    if (hlen < 0 || hlen >= (int)sizeof(header)) {
        proxy_conn_close(conn);
        return ESP_FAIL;
    }

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 15000);
        if (n <= 0) break;
        size_t copy = (total + (size_t)n < ab->cap - 1)
                      ? (size_t)n
                      : ab->cap - 1 - total;
        if (copy > 0) {
            memcpy(ab->data + total, tmp, copy);
            total += copy;
        }
    }
    ab->data[total] = '\0';
    ab->len = total;
    proxy_conn_close(conn);

    /* Check HTTP status line */
    int status = 0;
    if (total > 5 && strncmp(ab->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(ab->data, ' ');
        if (sp) status = atoi(sp + 1);
    }

    /* Strip HTTP headers */
    char *body = strstr(ab->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = total - (size_t)(body - ab->data);
        memmove(ab->data, body, blen);
        ab->len = blen;
        ab->data[ab->len] = '\0';
    }

    if (status != 200) {
        ESP_LOGE(TAG, "arXiv API returned HTTP %d via proxy", status);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_arxiv_execute(const char *input_json, char *output, size_t output_size)
{
    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mutex || !s_cache_val) {
        snprintf(output, output_size, "Error: arXiv tool not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Parse input JSON */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *keywords_j    = cJSON_GetObjectItem(input, "keywords");
    cJSON *start_j       = cJSON_GetObjectItem(input, "start");
    cJSON *max_results_j = cJSON_GetObjectItem(input, "max_results");

    if (!keywords_j || !cJSON_IsString(keywords_j) ||
        keywords_j->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'keywords' field");
        return ESP_ERR_INVALID_ARG;
    }

    int start       = (start_j && cJSON_IsNumber(start_j))
                      ? start_j->valueint : 0;
    int max_results = (max_results_j && cJSON_IsNumber(max_results_j))
                      ? max_results_j->valueint : ARXIV_DEFAULT_COUNT;

    if (start < 0) start = 0;
    if (max_results < 1) max_results = 1;
    if (max_results > ARXIV_MAX_COUNT) max_results = ARXIV_MAX_COUNT;

    ESP_LOGI(TAG, "arXiv search: \"%s\" start=%d max=%d",
             keywords_j->valuestring, start, max_results);

    /* Build cache key */
    char cache_key[ARXIV_CACHE_KEY_LEN];
    snprintf(cache_key, sizeof(cache_key), "%s|%d|%d",
             keywords_j->valuestring, start, max_results);

    /* Check cache (thread-safe) */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (strcmp(cache_key, s_cache_key) == 0 && s_cache_val[0] != '\0') {
        snprintf(output, output_size, "%s", s_cache_val);
        xSemaphoreGive(s_mutex);
        cJSON_Delete(input);
        ESP_LOGI(TAG, "Returning cached result");
        return ESP_OK;
    }
    xSemaphoreGive(s_mutex);

    /* Build URL path */
    char encoded_kw[512];
    url_encode(keywords_j->valuestring, encoded_kw, sizeof(encoded_kw));
    cJSON_Delete(input);

    char path[640];
    snprintf(path, sizeof(path),
             "/api/query?search_query=all:%s&start=%d&max_results=%d",
             encoded_kw, start, max_results);

    /* Allocate response buffer from PSRAM */
    arxiv_buf_t ab = {0};
    ab.data = heap_caps_calloc(1, ARXIV_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!ab.data) {
        snprintf(output, output_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }
    ab.cap = ARXIV_BUF_SIZE;

    /* Fetch from arXiv */
    esp_err_t err;
    if (http_proxy_is_enabled()) {
        err = arxiv_via_proxy(path, &ab);
    } else {
        char url[768];
        snprintf(url, sizeof(url), "https://export.arxiv.org%s", path);
        err = arxiv_direct(url, &ab);
    }

    if (err != ESP_OK) {
        heap_caps_free(ab.data);
        snprintf(output, output_size, "Error: arXiv request failed (%s)",
                 esp_err_to_name(err));
        return err;
    }

    /* Parse and format results */
    format_results(ab.data, output, output_size);
    heap_caps_free(ab.data);

    ESP_LOGI(TAG, "arXiv search complete, %d bytes result", (int)strlen(output));

    /* Update cache (thread-safe) */
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_cache_key, cache_key, ARXIV_CACHE_KEY_LEN - 1);
    strncpy(s_cache_val, output, ARXIV_CACHE_VAL_SIZE - 1);
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}
