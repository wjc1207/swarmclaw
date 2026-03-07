#include "tool_http_request.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

static const char *TAG = "http_request";

#define HTTP_BUF_SIZE       (16 * 1024)
#define HTTP_IMAGE_BUF_SIZE (200 * 1024)   /* 200KB for binary image data */
#define HTTP_TIMEOUT_MS     15000

/* ── Response accumulator ─────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} http_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_buf_t *hb = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t needed = hb->len + evt->data_len;
        if (needed + 1 < hb->cap) {
            memcpy(hb->data + hb->len, evt->data, evt->data_len);
            hb->len += evt->data_len;
            hb->data[hb->len] = '\0';
        }
    }
    return ESP_OK;
}

/* ── Direct HTTPS request ─────────────────────────────────────── */

static esp_err_t http_direct(const char *url, const char *method,
                             cJSON *headers, const char *body,
                             http_buf_t *hb, int *out_status)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = hb,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    /* Set method */
    if (strcmp(method, "POST") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
    } else if (strcmp(method, "PUT") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_PUT);
    } else if (strcmp(method, "DELETE") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_DELETE);
    } else if (strcmp(method, "PATCH") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_PATCH);
    } else if (strcmp(method, "HEAD") == 0) {
        esp_http_client_set_method(client, HTTP_METHOD_HEAD);
    }
    /* default: GET */

    /* Set custom headers */
    if (headers) {
        cJSON *h = NULL;
        cJSON_ArrayForEach(h, headers) {
            if (cJSON_IsString(h) && h->string) {
                esp_http_client_set_header(client, h->string, h->valuestring);
            }
        }
    }

    /* Set body */
    if (body && body[0]) {
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    return err;
}

/* ── Proxy HTTPS request ──────────────────────────────────────── */

static esp_err_t parse_url_parts(const char *url, char *host, size_t host_size,
                                 int *port, char *path, size_t path_size)
{
    /* Skip scheme */
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        *port = 443;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        *port = 80;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    /* Extract host */
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    size_t hlen;
    if (colon && (!slash || colon < slash)) {
        hlen = colon - p;
        *port = atoi(colon + 1);
    } else if (slash) {
        hlen = slash - p;
    } else {
        hlen = strlen(p);
    }

    if (hlen >= host_size) hlen = host_size - 1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    /* Extract path */
    if (slash) {
        strncpy(path, slash, path_size - 1);
        path[path_size - 1] = '\0';
    } else {
        strncpy(path, "/", path_size - 1);
    }

    return ESP_OK;
}

static esp_err_t http_via_proxy(const char *url, const char *method,
                                cJSON *headers, const char *body,
                                http_buf_t *hb, int *out_status)
{
    char host[128];
    char path[512];
    int port;

    if (parse_url_parts(url, host, sizeof(host), &port, path, sizeof(path)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }

    proxy_conn_t *conn = proxy_conn_open(host, port, HTTP_TIMEOUT_MS);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    /* Build request */
    size_t body_len = (body && body[0]) ? strlen(body) : 0;

    /* Build headers string */
    char hdr_extra[1024] = {0};
    size_t hdr_off = 0;
    if (headers) {
        cJSON *h = NULL;
        cJSON_ArrayForEach(h, headers) {
            if (cJSON_IsString(h) && h->string) {
                hdr_off += snprintf(hdr_extra + hdr_off, sizeof(hdr_extra) - hdr_off,
                    "%s: %s\r\n", h->string, h->valuestring);
            }
        }
    }

    char *req_buf = heap_caps_calloc(1, 2048 + body_len, MALLOC_CAP_SPIRAM);
    if (!req_buf) {
        proxy_conn_close(conn);
        return ESP_ERR_NO_MEM;
    }

    int rlen;
    if (body_len > 0) {
        char cl_header[48];
        snprintf(cl_header, sizeof(cl_header), "Content-Length: %d\r\n", (int)body_len);

        rlen = snprintf(req_buf, 2048 + body_len,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "%s"
            "%s"
            "Connection: close\r\n"
            "\r\n"
            "%s",
            method, path, host,
            hdr_extra,
            cl_header,
            body);
    } else {
        rlen = snprintf(req_buf, 2048,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "%s"
            "Connection: close\r\n"
            "\r\n",
            method, path, host,
            hdr_extra);
    }

    if (proxy_conn_write(conn, req_buf, rlen) < 0) {
        free(req_buf);
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }
    free(req_buf);

    /* Read full response */
    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), HTTP_TIMEOUT_MS);
        if (n <= 0) break;
        size_t copy = (total + n < hb->cap - 1) ? (size_t)n : hb->cap - 1 - total;
        if (copy > 0) {
            memcpy(hb->data + total, tmp, copy);
            total += copy;
        }
    }
    hb->data[total] = '\0';
    hb->len = total;
    proxy_conn_close(conn);

    /* Parse status code */
    *out_status = 0;
    if (total > 5 && strncmp(hb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(hb->data, ' ');
        if (sp) *out_status = atoi(sp + 1);
    }

    /* Strip headers, keep body */
    char *resp_body = strstr(hb->data, "\r\n\r\n");
    if (resp_body) {
        resp_body += 4;
        size_t blen = total - (resp_body - hb->data);
        memmove(hb->data, resp_body, blen);
        hb->len = blen;
        hb->data[hb->len] = '\0';
    }

    return ESP_OK;
}

/* ── Detect image media type from magic bytes ─────────────────── */

static const char *detect_media_type(const uint8_t *data, size_t len)
{
    if (len >= 2 && data[0] == 0xFF && data[1] == 0xD8)
        return "image/jpeg";
    if (len >= 4 && data[0] == 0x89 && data[1] == 0x50 &&
        data[2] == 0x4E && data[3] == 0x47)
        return "image/png";
    if (len >= 4 && data[0] == 'G' && data[1] == 'I' &&
        data[2] == 'F' && data[3] == '8')
        return "image/gif";
    if (len >= 12 && data[0] == 'R' && data[1] == 'I' &&
        data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' &&
        data[10] == 'B' && data[11] == 'P')
        return "image/webp";
    return "image/jpeg";   /* default assumption */
}

/* ── Validate method ──────────────────────────────────────────── */

static bool is_valid_method(const char *method)
{
    return strcmp(method, "GET") == 0 ||
           strcmp(method, "POST") == 0 ||
           strcmp(method, "PUT") == 0 ||
           strcmp(method, "DELETE") == 0 ||
           strcmp(method, "PATCH") == 0 ||
           strcmp(method, "HEAD") == 0;
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_http_request_execute(const char *input_json, char *output, size_t output_size)
{
    /* Parse input */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *url_item = cJSON_GetObjectItem(input, "url");
    if (!url_item || !cJSON_IsString(url_item) || url_item->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'url' field");
        return ESP_ERR_INVALID_ARG;
    }

    const char *url = url_item->valuestring;

    /* Validate URL scheme */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: URL must start with http:// or https://");
        return ESP_ERR_INVALID_ARG;
    }

    /* Method (default: GET) */
    const char *method = "GET";
    cJSON *method_item = cJSON_GetObjectItem(input, "method");
    if (method_item && cJSON_IsString(method_item) && method_item->valuestring[0]) {
        method = method_item->valuestring;
    }

    if (!is_valid_method(method)) {
        cJSON_Delete(input);
        snprintf(output, output_size,
            "Error: Unsupported method '%s'. Supported: GET, POST, PUT, DELETE, PATCH, HEAD", method);
        return ESP_ERR_INVALID_ARG;
    }

    /* Headers (optional object) */
    cJSON *headers = cJSON_GetObjectItem(input, "headers");

    /* Body (optional string) */
    cJSON *body_item = cJSON_GetObjectItem(input, "body");
    const char *body = (body_item && cJSON_IsString(body_item)) ? body_item->valuestring : NULL;

    ESP_LOGI(TAG, "HTTP %s %s", method, url);

    /* Check enable_image_analysis flag */
    cJSON *img_item = cJSON_GetObjectItem(input, "enable_image_analysis");
    bool enable_image = cJSON_IsTrue(img_item);

    /* Allocate response buffer from PSRAM (larger for image mode) */
    size_t buf_size = enable_image ? HTTP_IMAGE_BUF_SIZE : HTTP_BUF_SIZE;
    http_buf_t hb = {0};
    hb.data = heap_caps_calloc(1, buf_size, MALLOC_CAP_SPIRAM);
    if (!hb.data) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }
    hb.cap = buf_size;

    /* Make HTTP request */
    int status = 0;
    esp_err_t err;

    if (http_proxy_is_enabled()) {
        err = http_via_proxy(url, method, headers, body, &hb, &status);
    } else {
        err = http_direct(url, method, headers, body, &hb, &status);
    }

    cJSON_Delete(input);

    if (err != ESP_OK) {
        free(hb.data);
        snprintf(output, output_size, "Error: HTTP request failed (err=%d)", (int)err);
        return err;
    }

    if (enable_image) {
        /* ── Image analysis mode: base64-encode the response body ── */
        ESP_LOGI(TAG, "Image mode: %d bytes received", (int)hb.len);

        const char *media_type = detect_media_type((const uint8_t *)hb.data, hb.len);

        /* Write media type as first line */
        size_t mt_len = strlen(media_type);
        if (mt_len + 2 > output_size) {   /* media_type + '\n' + '\0' */
            free(hb.data);
            snprintf(output, output_size, "Error: Output buffer too small");
            return ESP_ERR_NO_MEM;
        }
        memcpy(output, media_type, mt_len);
        output[mt_len] = '\n';

        /* Base64-encode into remaining output buffer */
        size_t b64_len = 0;
        mbedtls_base64_encode(NULL, 0, &b64_len,
                              (const unsigned char *)hb.data, hb.len);

        if (mt_len + 1 + b64_len + 1 > output_size) {
            free(hb.data);
            snprintf(output, output_size, "Error: Image too large to base64-encode");
            ESP_LOGE(TAG, "Output buffer too small (%d needed)", (int)(mt_len + 1 + b64_len));
            return ESP_ERR_NO_MEM;
        }

        int ret = mbedtls_base64_encode(
            (unsigned char *)output + mt_len + 1,
            output_size - mt_len - 1,
            &b64_len,
            (const unsigned char *)hb.data,
            hb.len);

        free(hb.data);

        if (ret != 0) {
            snprintf(output, output_size, "Error: Base64 encoding failed");
            ESP_LOGE(TAG, "Base64 encode failed (ret=%d)", ret);
            return ESP_FAIL;
        }

        output[mt_len + 1 + b64_len] = '\0';
        ESP_LOGI(TAG, "Returning base64 image: %s, %d bytes", media_type, (int)b64_len);
        return ESP_OK;
    }

    /* ── Normal text mode ── */

    /* Format output */
    size_t off = snprintf(output, output_size, "Status: %d\n\n", status);

    /* Copy response body, truncate if needed */
    size_t remaining = output_size - off - 1;
    size_t body_copy = hb.len < remaining ? hb.len : remaining;
    if (body_copy > 0) {
        memcpy(output + off, hb.data, body_copy);
        output[off + body_copy] = '\0';
    }

    free(hb.data);

    ESP_LOGI(TAG, "HTTP %s complete, status=%d, %d bytes", method, status, (int)strlen(output));
    return ESP_OK;
}
