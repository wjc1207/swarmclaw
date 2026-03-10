#include "tool_registry.h"
#include "tools/tool_web_search.h"
#include "tools/tool_get_time.h"
#include "tools/tool_files.h"
#include "tools/tool_cron.h"
#include "tools/tool_gpio.h"
#include "tools/tool_http_request.h"
#include "tools/tool_script.h"

#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tools";

#define MAX_TOOLS 14

static mimi_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL;  /* cached JSON array string */

static void register_tool(const mimi_tool_t *tool)
{
    if (s_tool_count >= MAX_TOOLS) {
        ESP_LOGE(TAG, "Tool registry full");
        return;
    }
    s_tools[s_tool_count++] = *tool;
    ESP_LOGI(TAG, "Registered tool: %s", tool->name);
}

static void build_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);

        cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
    }

    free(s_tools_json);
    s_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Tools JSON built (%d tools)", s_tool_count);
}

esp_err_t tool_registry_init(void)
{
    s_tool_count = 0;

    /* Register web_search */
    tool_web_search_init();

    mimi_tool_t ws = {
        .name = "web_search",
        .description = "Search the web for current information. Use this when you need up-to-date facts, news, weather, or anything beyond your training data.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The search query\"}},"
            "\"required\":[\"query\"]}",
        .execute = tool_web_search_execute,
    };
    register_tool(&ws);

    /* Register get_current_time */
    mimi_tool_t gt = {
        .name = "get_current_time",
        .description = "Get the current date and time. Call this when you need to know what time or date it is.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_get_time_execute,
    };
    register_tool(&gt);

    /* Register read_file */
    mimi_tool_t rf = {
        .name = "read_file",
        .description = "Read a file from SPIFFS storage. Path must start with /spiffs/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with /spiffs/\"}},"
            "\"required\":[\"path\"]}",
        .execute = tool_read_file_execute,
    };
    register_tool(&rf);

    /* Register write_file */
    mimi_tool_t wf = {
        .name = "write_file",
        .description = "Write or overwrite a file on SPIFFS storage. Path must start with /spiffs/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with /spiffs/\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}},"
            "\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute,
    };
    register_tool(&wf);

    /* Register edit_file */
    mimi_tool_t ef = {
        .name = "edit_file",
        .description = "Find and replace text in a file on SPIFFS. Replaces first occurrence of old_string with new_string.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with /spiffs/\"},"
            "\"old_string\":{\"type\":\"string\",\"description\":\"Text to find\"},"
            "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
            "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file_execute,
    };
    register_tool(&ef);

    /* Register list_dir */
    mimi_tool_t ld = {
        .name = "list_dir",
        .description = "List files on SPIFFS storage, optionally filtered by path prefix.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"prefix\":{\"type\":\"string\",\"description\":\"Optional path prefix filter, e.g. /spiffs/memory/\"}},"
            "\"required\":[]}",
        .execute = tool_list_dir_execute,
    };
    register_tool(&ld);

    /* Register cron_add */
    mimi_tool_t ca = {
        .name = "cron_add",
        .description = "Schedule a recurring or one-shot task. The message will trigger an agent turn when the job fires.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short name for the job\"},"
            "\"schedule_type\":{\"type\":\"string\",\"description\":\"'every' for recurring interval or 'at' for one-shot at a unix timestamp\"},"
            "\"interval_s\":{\"type\":\"integer\",\"description\":\"Interval in seconds (required for 'every')\"},"
            "\"at_epoch\":{\"type\":\"integer\",\"description\":\"Unix timestamp to fire at (required for 'at')\"},"
            "\"message\":{\"type\":\"string\",\"description\":\"Message to inject when the job fires, triggering an agent turn\"},"
            "\"channel\":{\"type\":\"string\",\"description\":\"Optional reply channel (e.g. 'telegram'). Defaults to 'system'\"},"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Optional reply chat_id. Defaults to 'cron'\"}"
            "},"
            "\"required\":[\"name\",\"schedule_type\",\"message\"]}",
        .execute = tool_cron_add_execute,
    };
    register_tool(&ca);

    /* Register cron_list */
    mimi_tool_t cl = {
        .name = "cron_list",
        .description = "List all scheduled cron jobs with their status, schedule, and IDs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_cron_list_execute,
    };
    register_tool(&cl);

    /* Register cron_remove */
    mimi_tool_t cr = {
        .name = "cron_remove",
        .description = "Remove a scheduled cron job by its ID.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"The 8-character job ID to remove\"}},"
            "\"required\":[\"job_id\"]}",
        .execute = tool_cron_remove_execute,
    };
    register_tool(&cr);

    /* Register gpio — unified hardware I/O tool */
    tool_gpio_init();

    mimi_tool_t gpio = {
        .name = "gpio",
        .description = "Unified hardware I/O tool. Controls GPIO pins, I2C, SPI, RGB LEDs, PWM, UART, and 1-Wire via a single 'action' field. "
                       "Actions: gpio_set_dir, gpio_write, gpio_read, gpio_set_pull, gpio_on_edge, "
                       "i2c_write, i2c_read, i2c_write_read, spi_transfer, spi_write, "
                       "rgb_set_pixel, rgb_fill, rgb_set_range, rgb_show, rgb_clear, "
                       "pwm_start, pwm_set_duty, pwm_set_freq, pwm_stop, "
                       "uart_write, uart_read, onewire_scan, onewire_read. "
                       "For the onboard WS2812 RGB LED on GPIO48 use action 'set_rgb' with r, g, b.",
        .input_schema_json =
            "{"
            "\"type\":\"object\","
            "\"properties\":{"
                "\"action\":{\"type\":\"string\",\"description\":\"The operation to perform\"},"
                "\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"},"
                "\"direction\":{\"type\":\"string\",\"description\":\"Pin direction: IN or OUT\"},"
                "\"value\":{\"description\":\"Pin value: 0, 1, HIGH, or LOW\"},"
                "\"pull\":{\"type\":\"string\",\"description\":\"Pull resistor: UP, DOWN, or NONE\"},"
                "\"edge\":{\"type\":\"string\",\"description\":\"Edge type: RISING, FALLING, or BOTH\"},"
                "\"sda\":{\"type\":\"integer\",\"description\":\"I2C SDA pin\"},"
                "\"scl\":{\"type\":\"integer\",\"description\":\"I2C SCL pin\"},"
                "\"addr\":{\"type\":\"integer\",\"description\":\"I2C device address\"},"
                "\"data\":{\"type\":\"array\",\"description\":\"Byte array for write operations\",\"items\":{\"type\":\"integer\"}},"
                "\"length\":{\"type\":\"integer\",\"description\":\"Number of bytes to read\"},"
                "\"freq\":{\"type\":\"integer\",\"description\":\"I2C frequency in Hz (default 100000)\"},"
                "\"mosi\":{\"type\":\"integer\",\"description\":\"SPI MOSI pin\"},"
                "\"miso\":{\"type\":\"integer\",\"description\":\"SPI MISO pin\"},"
                "\"sclk\":{\"type\":\"integer\",\"description\":\"SPI SCLK pin\"},"
                "\"cs\":{\"type\":\"integer\",\"description\":\"SPI CS pin\"},"
                "\"tx\":{\"description\":\"SPI/UART transmit data or UART TX pin\"},"
                "\"rx\":{\"type\":\"integer\",\"description\":\"UART RX pin\"},"
                "\"mode\":{\"type\":\"integer\",\"description\":\"SPI mode 0-3\"},"
                "\"speed\":{\"type\":\"integer\",\"description\":\"SPI clock speed Hz\"},"
                "\"num_pixels\":{\"type\":\"integer\",\"description\":\"Number of LEDs in strip\"},"
                "\"index\":{\"type\":\"integer\",\"description\":\"LED pixel index\"},"
                "\"r\":{\"type\":\"integer\",\"description\":\"Red 0-255\"},"
                "\"g\":{\"type\":\"integer\",\"description\":\"Green 0-255\"},"
                "\"b\":{\"type\":\"integer\",\"description\":\"Blue 0-255\"},"
                "\"start\":{\"type\":\"integer\",\"description\":\"Range start index\"},"
                "\"end\":{\"type\":\"integer\",\"description\":\"Range end index\"},"
                "\"brightness\":{\"type\":\"number\",\"description\":\"Brightness 0.0-1.0\"},"
                "\"duty\":{\"type\":\"integer\",\"description\":\"PWM duty cycle 0-100\"},"
                "\"baud\":{\"type\":\"integer\",\"description\":\"UART baud rate\"},"
                "\"timeout\":{\"type\":\"number\",\"description\":\"UART read timeout in seconds\"},"
                "\"rom\":{\"type\":\"string\",\"description\":\"1-Wire ROM address hex string\"},"
                "\"command\":{\"type\":\"integer\",\"description\":\"1-Wire device command byte\"}"
            "},"
            "\"required\":[\"action\"]"
            "}",
        .execute = tool_gpio_execute,
    };

    register_tool(&gpio);

 /* Register http_request */
    mimi_tool_t hr = {
        .name = "http_request",
        .description = "Make HTTP requests to external APIs and websites. Supports GET, POST, PUT, DELETE, PATCH, HEAD methods. Use for API calls, fetching data from URLs, etc. Set enable_image_analysis to true to capture an image from a URL and return it as base64 for vision analysis.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"url\":{\"type\":\"string\",\"description\":\"HTTP or HTTPS URL to request\"},"
            "\"method\":{\"type\":\"string\",\"description\":\"HTTP method: GET, POST, PUT, DELETE, PATCH, HEAD (default: GET)\"},"
            "\"headers\":{\"type\":\"object\",\"description\":\"Optional HTTP headers as key-value pairs\"},"
            "\"body\":{\"type\":\"string\",\"description\":\"Optional request body (for POST, PUT, PATCH)\"},"
            "\"enable_image_analysis\":{\"type\":\"boolean\",\"description\":\"When true, treat the response as an image, base64-encode it, and return it for LLM vision analysis\"}"
            "},"
            "\"required\":[\"url\"]}",
        .execute = tool_http_request_execute,
    };
    register_tool(&hr);

    /* Register script_write */
    mimi_tool_t sw = {
        .name = "script_write",
        .description = "Write a Python script to SPIFFS. The script can import gpio, i2c, spi, rgb, pwm, time_ms modules.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"e.g. /spiffs/scripts/blink.py\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"Full Python source code\"}"
            "},"
            "\"required\":[\"path\",\"content\"]}",
        .execute = tool_script_write_execute,
    };
    register_tool(&sw);

    /* Register script_run */
    mimi_tool_t sr = {
        .name = "script_run",
        .description = "Execute a Python script on the ESP32-S3. Returns stdout output or error message.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"e.g. /spiffs/scripts/blink.py\"},"
            "\"timeout_ms\":{\"type\":\"integer\",\"description\":\"Max execution time in ms (default 5000)\"}"
            "},"
            "\"required\":[\"path\"]}",
        .execute = tool_script_run_execute,
    };
    register_tool(&sr);

    // After registering all tools, build the JSON array string
    build_tools_json();

    ESP_LOGI(TAG, "Tool registry initialized");
    return ESP_OK;
}

const char *tool_registry_get_tools_json(void)
{
    return s_tools_json;
}

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size)
{
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            ESP_LOGI(TAG, "Executing tool: %s", name);
            return s_tools[i].execute(input_json, output, output_size);
        }
    }

    ESP_LOGW(TAG, "Unknown tool: %s", name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ESP_ERR_NOT_FOUND;
}
