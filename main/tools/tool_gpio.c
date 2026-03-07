#include "tool_gpio.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/uart.h"
#include "led_strip.h"

static const char *TAG = "tool_gpio";

/* ====================================================================
   Constants
   ==================================================================== */
#define GPIO_PIN_MAX        49          /* ESP32-S3: GPIO 0-48           */
#define MAX_I2C_BUSES       2
#define MAX_SPI_BUSES       2
#define MAX_RGB_STRIPS      4
#define MAX_PWM_CHANNELS    8
#define MAX_UART_PORTS      2           /* UART1 and UART2 for users     */
#define RMT_RESOLUTION      (10 * 1000 * 1000) /* 10 MHz */
#define LEDC_DUTY_MAX       8191    /* 2^13 - 1, for LEDC_TIMER_13_BIT */
#define RGB_FILE_PATH       "/spiffs/rgb.txt"
#define I2C_TIMEOUT_MS      1000
#define MAX_TRANSFER_BYTES  256

/* ====================================================================
   Pin-usage tracking
   ==================================================================== */
typedef enum {
    PIN_FREE = 0,
    PIN_GPIO,
    PIN_I2C,
    PIN_SPI,
    PIN_RGB,
    PIN_PWM,
    PIN_UART,
    PIN_ONEWIRE,
} pin_usage_t;

static pin_usage_t  s_pin_usage[GPIO_PIN_MAX];
static SemaphoreHandle_t s_mutex = NULL;
static bool s_isr_service_installed = false;

/* ====================================================================
   Cached protocol handles
   ==================================================================== */

/* --- I2C ----------------------------------------------------------- */
typedef struct {
    int  sda;
    int  scl;
    i2c_master_bus_handle_t bus;
} i2c_bus_entry_t;

static i2c_bus_entry_t s_i2c[MAX_I2C_BUSES];
static int             s_i2c_count = 0;

/* --- SPI ----------------------------------------------------------- */
typedef struct {
    int  mosi;
    int  miso;
    int  sclk;
    spi_host_device_t host;
} spi_bus_entry_t;

static spi_bus_entry_t s_spi[MAX_SPI_BUSES];
static int             s_spi_count = 0;

/* --- RGB ----------------------------------------------------------- */
typedef struct {
    int  pin;
    int  num_pixels;
    led_strip_handle_t strip;
} rgb_entry_t;

static rgb_entry_t s_rgb[MAX_RGB_STRIPS];
static int         s_rgb_count = 0;

/* --- PWM ----------------------------------------------------------- */
typedef struct {
    int  pin;
    ledc_channel_t channel;
    ledc_timer_t   timer;
    bool active;
} pwm_entry_t;

static pwm_entry_t s_pwm[MAX_PWM_CHANNELS];
static int         s_pwm_count = 0;

/* --- UART ---------------------------------------------------------- */
typedef struct {
    int  tx;
    int  rx;
    uart_port_t port;
} uart_entry_t;

static uart_entry_t s_uart[MAX_UART_PORTS];
static int          s_uart_count = 0;

/* ====================================================================
   JSON response helpers
   ==================================================================== */
static void reply_ok(char *out, size_t sz, const char *result)
{
    if (result)
        snprintf(out, sz, "{\"ok\":true,\"result\":%s}", result);
    else
        snprintf(out, sz, "{\"ok\":true,\"result\":null}");
}

static void reply_ok_int(char *out, size_t sz, int value)
{
    snprintf(out, sz, "{\"ok\":true,\"result\":%d}", value);
}

static void reply_ok_str(char *out, size_t sz, const char *str)
{
    snprintf(out, sz, "{\"ok\":true,\"result\":\"%s\"}", str);
}

static void reply_error(char *out, size_t sz, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    snprintf(out, sz, "{\"ok\":false,\"error\":\"%s\"}", msg);
}

/* ====================================================================
   Pin management
   ==================================================================== */
static bool pin_valid(int pin)
{
    return pin >= 0 && pin < GPIO_PIN_MAX;
}

static bool pin_claim(int pin, pin_usage_t usage, char *out, size_t sz)
{
    if (!pin_valid(pin)) {
        reply_error(out, sz, "Invalid pin %d (must be 0-%d)", pin, GPIO_PIN_MAX - 1);
        return false;
    }
    if (s_pin_usage[pin] != PIN_FREE && s_pin_usage[pin] != usage) {
        const char *names[] = {
            "free", "GPIO", "I2C", "SPI", "RGB", "PWM", "UART", "1-Wire"
        };
        reply_error(out, sz, "Pin %d already in use by %s", pin, names[s_pin_usage[pin]]);
        return false;
    }
    s_pin_usage[pin] = usage;
    return true;
}

static void pin_release(int pin)
{
    if (pin_valid(pin)) s_pin_usage[pin] = PIN_FREE;
}

/* ====================================================================
   Helper: parse JSON int array into byte buffer
   ==================================================================== */
static int parse_byte_array(cJSON *arr, uint8_t *buf, int max_len)
{
    if (!cJSON_IsArray(arr)) return -1;
    int n = cJSON_GetArraySize(arr);
    if (n > max_len) n = max_len;
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (cJSON_IsNumber(item))
            buf[i] = (uint8_t)item->valueint;
    }
    return n;
}

/* helper: format byte array as JSON */
static int format_byte_array(const uint8_t *data, int len, char *buf, size_t sz)
{
    int pos = 0;
    pos += snprintf(buf + pos, sz - pos, "[");
    for (int i = 0; i < len && (size_t)pos < sz - 6; i++) {
        if (i > 0) pos += snprintf(buf + pos, sz - pos, ",");
        pos += snprintf(buf + pos, sz - pos, "%d", data[i]);
    }
    pos += snprintf(buf + pos, sz - pos, "]");
    return pos;
}

/* ====================================================================
   GPIO — Raw pin control
   ==================================================================== */
static esp_err_t handle_gpio_set_dir(cJSON *root, char *out, size_t sz)
{
    int pin = cJSON_GetObjectItem(root, "pin") ?
              cJSON_GetObjectItem(root, "pin")->valueint : -1;
    const char *dir = cJSON_GetStringValue(cJSON_GetObjectItem(root, "direction"));
    if (pin < 0 || !dir) {
        reply_error(out, sz, "gpio_set_dir requires pin and direction");
        return ESP_ERR_INVALID_ARG;
    }
    if (!pin_claim(pin, PIN_GPIO, out, sz)) return ESP_ERR_INVALID_STATE;

    gpio_mode_t mode = GPIO_MODE_INPUT;
    if (strcmp(dir, "OUT") == 0)      mode = GPIO_MODE_OUTPUT;
    else if (strcmp(dir, "IN") == 0)  mode = GPIO_MODE_INPUT;
    else {
        reply_error(out, sz, "direction must be IN or OUT");
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        reply_error(out, sz, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    reply_ok(out, sz, NULL);
    return ESP_OK;
}

static esp_err_t handle_gpio_write(cJSON *root, char *out, size_t sz)
{
    int pin = cJSON_GetObjectItem(root, "pin") ?
              cJSON_GetObjectItem(root, "pin")->valueint : -1;
    if (pin < 0) {
        reply_error(out, sz, "gpio_write requires pin");
        return ESP_ERR_INVALID_ARG;
    }
    if (!pin_claim(pin, PIN_GPIO, out, sz)) return ESP_ERR_INVALID_STATE;

    int level = 0;
    cJSON *val = cJSON_GetObjectItem(root, "value");
    if (cJSON_IsNumber(val)) {
        level = val->valueint ? 1 : 0;
    } else if (cJSON_IsString(val)) {
        const char *s = val->valuestring;
        if (strcmp(s, "HIGH") == 0 || strcmp(s, "1") == 0) level = 1;
    }

    esp_err_t err = gpio_set_level((gpio_num_t)pin, level);
    if (err != ESP_OK) {
        reply_error(out, sz, "gpio_set_level failed: %s", esp_err_to_name(err));
        return err;
    }
    reply_ok(out, sz, NULL);
    return ESP_OK;
}

static esp_err_t handle_gpio_read(cJSON *root, char *out, size_t sz)
{
    int pin = cJSON_GetObjectItem(root, "pin") ?
              cJSON_GetObjectItem(root, "pin")->valueint : -1;
    if (pin < 0) {
        reply_error(out, sz, "gpio_read requires pin");
        return ESP_ERR_INVALID_ARG;
    }
    if (!pin_claim(pin, PIN_GPIO, out, sz)) return ESP_ERR_INVALID_STATE;

    int level = gpio_get_level((gpio_num_t)pin);
    reply_ok_int(out, sz, level);
    return ESP_OK;
}

static esp_err_t handle_gpio_set_pull(cJSON *root, char *out, size_t sz)
{
    int pin = cJSON_GetObjectItem(root, "pin") ?
              cJSON_GetObjectItem(root, "pin")->valueint : -1;
    const char *pull = cJSON_GetStringValue(cJSON_GetObjectItem(root, "pull"));
    if (pin < 0 || !pull) {
        reply_error(out, sz, "gpio_set_pull requires pin and pull");
        return ESP_ERR_INVALID_ARG;
    }
    if (!pin_claim(pin, PIN_GPIO, out, sz)) return ESP_ERR_INVALID_STATE;

    gpio_pull_mode_t mode = GPIO_FLOATING;
    if (strcmp(pull, "UP") == 0)        mode = GPIO_PULLUP_ONLY;
    else if (strcmp(pull, "DOWN") == 0) mode = GPIO_PULLDOWN_ONLY;
    else if (strcmp(pull, "NONE") == 0) mode = GPIO_FLOATING;
    else {
        reply_error(out, sz, "pull must be UP, DOWN, or NONE");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = gpio_set_pull_mode((gpio_num_t)pin, mode);
    if (err != ESP_OK) {
        reply_error(out, sz, "gpio_set_pull_mode failed: %s", esp_err_to_name(err));
        return err;
    }
    reply_ok(out, sz, NULL);
    return ESP_OK;
}

/* GPIO edge interrupt — stores last edge count; no user callback in C tool */
static volatile int s_edge_count[GPIO_PIN_MAX];

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    int pin = (int)(intptr_t)arg;
    if (pin >= 0 && pin < GPIO_PIN_MAX) s_edge_count[pin]++;
}

static esp_err_t handle_gpio_on_edge(cJSON *root, char *out, size_t sz)
{
    int pin = cJSON_GetObjectItem(root, "pin") ?
              cJSON_GetObjectItem(root, "pin")->valueint : -1;
    const char *edge = cJSON_GetStringValue(cJSON_GetObjectItem(root, "edge"));
    if (pin < 0 || !edge) {
        reply_error(out, sz, "gpio_on_edge requires pin and edge");
        return ESP_ERR_INVALID_ARG;
    }
    if (!pin_claim(pin, PIN_GPIO, out, sz)) return ESP_ERR_INVALID_STATE;

    gpio_int_type_t intr = GPIO_INTR_ANYEDGE;
    if (strcmp(edge, "RISING") == 0)       intr = GPIO_INTR_POSEDGE;
    else if (strcmp(edge, "FALLING") == 0) intr = GPIO_INTR_NEGEDGE;
    else if (strcmp(edge, "BOTH") == 0)    intr = GPIO_INTR_ANYEDGE;

    if (!s_isr_service_installed) {
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            reply_error(out, sz, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
            return err;
        }
        s_isr_service_installed = true;
    }

    s_edge_count[pin] = 0;
    esp_err_t err = gpio_set_intr_type((gpio_num_t)pin, intr);
    if (err == ESP_OK) err = gpio_isr_handler_add((gpio_num_t)pin, gpio_isr_handler, (void *)(intptr_t)pin);
    if (err != ESP_OK) {
        reply_error(out, sz, "gpio_on_edge failed: %s", esp_err_to_name(err));
        return err;
    }
    reply_ok(out, sz, NULL);
    return ESP_OK;
}

/* ====================================================================
   I²C
   ==================================================================== */
static i2c_master_bus_handle_t i2c_get_bus(int sda, int scl, char *out, size_t sz)
{
    /* Return cached bus if pins match */
    for (int i = 0; i < s_i2c_count; i++) {
        if (s_i2c[i].sda == sda && s_i2c[i].scl == scl)
            return s_i2c[i].bus;
    }
    if (s_i2c_count >= MAX_I2C_BUSES) {
        reply_error(out, sz, "No free I2C bus slot (max %d)", MAX_I2C_BUSES);
        return NULL;
    }
    if (!pin_claim(sda, PIN_I2C, out, sz)) return NULL;
    if (!pin_claim(scl, PIN_I2C, out, sz)) return NULL;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = (s_i2c_count == 0) ? I2C_NUM_0 : I2C_NUM_1,
        .sda_io_num = (gpio_num_t)sda,
        .scl_io_num = (gpio_num_t)scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        reply_error(out, sz, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        pin_release(sda);
        pin_release(scl);
        return NULL;
    }
    s_i2c[s_i2c_count].sda = sda;
    s_i2c[s_i2c_count].scl = scl;
    s_i2c[s_i2c_count].bus = bus;
    s_i2c_count++;
    return bus;
}

static esp_err_t handle_i2c_write(cJSON *root, char *out, size_t sz)
{
    int sda  = cJSON_GetObjectItem(root, "sda")  ? cJSON_GetObjectItem(root, "sda")->valueint  : -1;
    int scl  = cJSON_GetObjectItem(root, "scl")  ? cJSON_GetObjectItem(root, "scl")->valueint  : -1;
    int addr = cJSON_GetObjectItem(root, "addr") ? cJSON_GetObjectItem(root, "addr")->valueint : -1;
    cJSON *jdata = cJSON_GetObjectItem(root, "data");
    int freq = 100000;
    cJSON *jf = cJSON_GetObjectItem(root, "freq");
    if (cJSON_IsNumber(jf)) freq = jf->valueint;

    if (sda < 0 || scl < 0 || addr < 0 || !jdata) {
        reply_error(out, sz, "i2c_write requires sda, scl, addr, data");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[MAX_TRANSFER_BYTES];
    int len = parse_byte_array(jdata, buf, MAX_TRANSFER_BYTES);
    if (len <= 0) {
        reply_error(out, sz, "data must be a non-empty array of integers");
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_handle_t bus = i2c_get_bus(sda, scl, out, sz);
    if (!bus) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (uint16_t)addr,
        .scl_speed_hz = (uint32_t)freq,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        reply_error(out, sz, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_master_transmit(dev, buf, (size_t)len, I2C_TIMEOUT_MS);
    i2c_master_bus_rm_device(dev);

    if (err != ESP_OK) {
        reply_error(out, sz, "i2c_master_transmit failed: %s", esp_err_to_name(err));
        return err;
    }
    reply_ok(out, sz, NULL);
    return ESP_OK;
}

static esp_err_t handle_i2c_read(cJSON *root, char *out, size_t sz)
{
    int sda    = cJSON_GetObjectItem(root, "sda")    ? cJSON_GetObjectItem(root, "sda")->valueint    : -1;
    int scl    = cJSON_GetObjectItem(root, "scl")    ? cJSON_GetObjectItem(root, "scl")->valueint    : -1;
    int addr   = cJSON_GetObjectItem(root, "addr")   ? cJSON_GetObjectItem(root, "addr")->valueint   : -1;
    int length = cJSON_GetObjectItem(root, "length") ? cJSON_GetObjectItem(root, "length")->valueint : -1;
    int freq   = 100000;
    cJSON *jf = cJSON_GetObjectItem(root, "freq");
    if (cJSON_IsNumber(jf)) freq = jf->valueint;

    if (sda < 0 || scl < 0 || addr < 0 || length <= 0) {
        reply_error(out, sz, "i2c_read requires sda, scl, addr, length");
        return ESP_ERR_INVALID_ARG;
    }
    if (length > MAX_TRANSFER_BYTES) length = MAX_TRANSFER_BYTES;

    i2c_master_bus_handle_t bus = i2c_get_bus(sda, scl, out, sz);
    if (!bus) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (uint16_t)addr,
        .scl_speed_hz = (uint32_t)freq,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        reply_error(out, sz, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t rbuf[MAX_TRANSFER_BYTES];
    err = i2c_master_receive(dev, rbuf, (size_t)length, I2C_TIMEOUT_MS);
    i2c_master_bus_rm_device(dev);

    if (err != ESP_OK) {
        reply_error(out, sz, "i2c_master_receive failed: %s", esp_err_to_name(err));
        return err;
    }

    char arr[MAX_TRANSFER_BYTES * 4 + 4];
    format_byte_array(rbuf, length, arr, sizeof(arr));
    reply_ok(out, sz, arr);
    return ESP_OK;
}

static esp_err_t handle_i2c_write_read(cJSON *root, char *out, size_t sz)
{
    int sda    = cJSON_GetObjectItem(root, "sda")    ? cJSON_GetObjectItem(root, "sda")->valueint    : -1;
    int scl    = cJSON_GetObjectItem(root, "scl")    ? cJSON_GetObjectItem(root, "scl")->valueint    : -1;
    int addr   = cJSON_GetObjectItem(root, "addr")   ? cJSON_GetObjectItem(root, "addr")->valueint   : -1;
    int length = cJSON_GetObjectItem(root, "length") ? cJSON_GetObjectItem(root, "length")->valueint : -1;
    cJSON *jdata = cJSON_GetObjectItem(root, "data");
    int freq   = 100000;
    cJSON *jf = cJSON_GetObjectItem(root, "freq");
    if (cJSON_IsNumber(jf)) freq = jf->valueint;

    if (sda < 0 || scl < 0 || addr < 0 || !jdata || length <= 0) {
        reply_error(out, sz, "i2c_write_read requires sda, scl, addr, data, length");
        return ESP_ERR_INVALID_ARG;
    }
    if (length > MAX_TRANSFER_BYTES) length = MAX_TRANSFER_BYTES;

    uint8_t wbuf[MAX_TRANSFER_BYTES];
    int wlen = parse_byte_array(jdata, wbuf, MAX_TRANSFER_BYTES);
    if (wlen <= 0) {
        reply_error(out, sz, "data must be a non-empty array of integers");
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_handle_t bus = i2c_get_bus(sda, scl, out, sz);
    if (!bus) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = (uint16_t)addr,
        .scl_speed_hz = (uint32_t)freq,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        reply_error(out, sz, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t rbuf[MAX_TRANSFER_BYTES];
    err = i2c_master_transmit_receive(dev, wbuf, (size_t)wlen,
                                      rbuf, (size_t)length, I2C_TIMEOUT_MS);
    i2c_master_bus_rm_device(dev);

    if (err != ESP_OK) {
        reply_error(out, sz, "i2c_master_transmit_receive failed: %s", esp_err_to_name(err));
        return err;
    }

    char arr[MAX_TRANSFER_BYTES * 4 + 4];
    format_byte_array(rbuf, length, arr, sizeof(arr));
    reply_ok(out, sz, arr);
    return ESP_OK;
}

/* ====================================================================
   SPI
   ==================================================================== */
static int spi_get_bus_index(int mosi, int miso, int sclk, char *out, size_t sz)
{
    for (int i = 0; i < s_spi_count; i++) {
        if (s_spi[i].mosi == mosi && s_spi[i].sclk == sclk &&
            s_spi[i].miso == miso)
            return i;
    }
    if (s_spi_count >= MAX_SPI_BUSES) {
        reply_error(out, sz, "No free SPI bus slot (max %d)", MAX_SPI_BUSES);
        return -1;
    }

    if (!pin_claim(mosi, PIN_SPI, out, sz)) return -1;
    if (!pin_claim(sclk, PIN_SPI, out, sz)) return -1;
    if (miso >= 0 && !pin_claim(miso, PIN_SPI, out, sz)) return -1;

    spi_host_device_t host = (s_spi_count == 0) ? SPI2_HOST : SPI3_HOST;
    spi_bus_config_t buscfg = {
        .mosi_io_num = mosi,
        .miso_io_num = (miso >= 0) ? miso : -1,
        .sclk_io_num = sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MAX_TRANSFER_BYTES,
    };
    esp_err_t err = spi_bus_initialize(host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        reply_error(out, sz, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return -1;
    }

    int idx = s_spi_count;
    s_spi[idx].mosi = mosi;
    s_spi[idx].miso = miso;
    s_spi[idx].sclk = sclk;
    s_spi[idx].host = host;
    s_spi_count++;
    return idx;
}

static esp_err_t handle_spi_transfer(cJSON *root, char *out, size_t sz)
{
    int mosi  = cJSON_GetObjectItem(root, "mosi") ? cJSON_GetObjectItem(root, "mosi")->valueint : -1;
    int miso  = cJSON_GetObjectItem(root, "miso") ? cJSON_GetObjectItem(root, "miso")->valueint : -1;
    int sclk  = cJSON_GetObjectItem(root, "sclk") ? cJSON_GetObjectItem(root, "sclk")->valueint : -1;
    int cs    = cJSON_GetObjectItem(root, "cs")   ? cJSON_GetObjectItem(root, "cs")->valueint   : -1;
    cJSON *jtx = cJSON_GetObjectItem(root, "tx");
    int mode  = 0, speed = 1000000;
    cJSON *jm = cJSON_GetObjectItem(root, "mode");
    cJSON *js = cJSON_GetObjectItem(root, "speed");
    if (cJSON_IsNumber(jm)) mode  = jm->valueint;
    if (cJSON_IsNumber(js)) speed = js->valueint;

    if (mosi < 0 || miso < 0 || sclk < 0 || cs < 0 || !jtx) {
        reply_error(out, sz, "spi_transfer requires mosi, miso, sclk, cs, tx");
        return ESP_ERR_INVALID_ARG;
    }
    if (!pin_claim(cs, PIN_SPI, out, sz)) return ESP_ERR_INVALID_STATE;

    uint8_t tx_buf[MAX_TRANSFER_BYTES], rx_buf[MAX_TRANSFER_BYTES];
    int len = parse_byte_array(jtx, tx_buf, MAX_TRANSFER_BYTES);
    if (len <= 0) {
        reply_error(out, sz, "tx must be a non-empty array of integers");
        return ESP_ERR_INVALID_ARG;
    }
    memset(rx_buf, 0, sizeof(rx_buf));

    int idx = spi_get_bus_index(mosi, miso, sclk, out, sz);
    if (idx < 0) return ESP_ERR_INVALID_STATE;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = speed,
        .mode = mode,
        .spics_io_num = cs,
        .queue_size = 1,
    };
    spi_device_handle_t dev;
    esp_err_t err = spi_bus_add_device(s_spi[idx].host, &devcfg, &dev);
    if (err != ESP_OK) {
        reply_error(out, sz, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx_buf,
        .rx_buffer = rx_buf,
    };
    err = spi_device_polling_transmit(dev, &t);
    spi_bus_remove_device(dev);

    if (err != ESP_OK) {
        reply_error(out, sz, "spi_device_polling_transmit failed: %s", esp_err_to_name(err));
        return err;
    }

    char arr[MAX_TRANSFER_BYTES * 4 + 4];
    format_byte_array(rx_buf, len, arr, sizeof(arr));
    reply_ok(out, sz, arr);
    return ESP_OK;
}

static esp_err_t handle_spi_write(cJSON *root, char *out, size_t sz)
{
    int mosi = cJSON_GetObjectItem(root, "mosi") ? cJSON_GetObjectItem(root, "mosi")->valueint : -1;
    int sclk = cJSON_GetObjectItem(root, "sclk") ? cJSON_GetObjectItem(root, "sclk")->valueint : -1;
    int cs   = cJSON_GetObjectItem(root, "cs")   ? cJSON_GetObjectItem(root, "cs")->valueint   : -1;
    cJSON *jtx = cJSON_GetObjectItem(root, "tx");
    int mode  = 0, speed = 1000000;
    cJSON *jm = cJSON_GetObjectItem(root, "mode");
    cJSON *js = cJSON_GetObjectItem(root, "speed");
    if (cJSON_IsNumber(jm)) mode  = jm->valueint;
    if (cJSON_IsNumber(js)) speed = js->valueint;

    if (mosi < 0 || sclk < 0 || cs < 0 || !jtx) {
        reply_error(out, sz, "spi_write requires mosi, sclk, cs, tx");
        return ESP_ERR_INVALID_ARG;
    }
    if (!pin_claim(cs, PIN_SPI, out, sz)) return ESP_ERR_INVALID_STATE;

    uint8_t tx_buf[MAX_TRANSFER_BYTES];
    int len = parse_byte_array(jtx, tx_buf, MAX_TRANSFER_BYTES);
    if (len <= 0) {
        reply_error(out, sz, "tx must be a non-empty array of integers");
        return ESP_ERR_INVALID_ARG;
    }

    /* miso = -1 for write-only */
    int idx = spi_get_bus_index(mosi, -1, sclk, out, sz);
    if (idx < 0) return ESP_ERR_INVALID_STATE;

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = speed,
        .mode = mode,
        .spics_io_num = cs,
        .queue_size = 1,
    };
    spi_device_handle_t dev;
    esp_err_t err = spi_bus_add_device(s_spi[idx].host, &devcfg, &dev);
    if (err != ESP_OK) {
        reply_error(out, sz, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx_buf,
    };
    err = spi_device_polling_transmit(dev, &t);
    spi_bus_remove_device(dev);

    if (err != ESP_OK) {
        reply_error(out, sz, "spi_device_polling_transmit failed: %s", esp_err_to_name(err));
        return err;
    }
    reply_ok(out, sz, NULL);
    return ESP_OK;
}

/* ====================================================================
   RGB — Addressable LED strips (WS2812B)
   ==================================================================== */
static led_strip_handle_t rgb_get_strip(int pin, int num_pixels, char *out, size_t sz)
{
    for (int i = 0; i < s_rgb_count; i++) {
        if (s_rgb[i].pin == pin) {
            /* Resize if needed */
            if (s_rgb[i].num_pixels < num_pixels) {
                led_strip_del(s_rgb[i].strip);
                led_strip_config_t cfg = { .strip_gpio_num = pin, .max_leds = (uint32_t)num_pixels };
                led_strip_rmt_config_t rmt = {
                    .clk_src = RMT_CLK_SRC_DEFAULT,
                    .resolution_hz = RMT_RESOLUTION,
                    .mem_block_symbols = 64,
                    .flags.with_dma = false,
                };
                if (led_strip_new_rmt_device(&cfg, &rmt, &s_rgb[i].strip) != ESP_OK) {
                    reply_error(out, sz, "led_strip_new_rmt_device resize failed");
                    return NULL;
                }
                s_rgb[i].num_pixels = num_pixels;
            }
            return s_rgb[i].strip;
        }
    }

    if (s_rgb_count >= MAX_RGB_STRIPS) {
        reply_error(out, sz, "No free RGB slot (max %d)", MAX_RGB_STRIPS);
        return NULL;
    }
    if (!pin_claim(pin, PIN_RGB, out, sz)) return NULL;

    led_strip_config_t cfg = { .strip_gpio_num = pin, .max_leds = (uint32_t)num_pixels };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION,
        .mem_block_symbols = 64,
        .flags.with_dma = false,
    };
    led_strip_handle_t strip = NULL;
    esp_err_t err = led_strip_new_rmt_device(&cfg, &rmt, &strip);
    if (err != ESP_OK) {
        reply_error(out, sz, "led_strip_new_rmt_device failed: %s", esp_err_to_name(err));
        pin_release(pin);
        return NULL;
    }

    s_rgb[s_rgb_count].pin = pin;
    s_rgb[s_rgb_count].num_pixels = num_pixels;
    s_rgb[s_rgb_count].strip = strip;
    s_rgb_count++;
    ESP_LOGI(TAG, "RGB strip initialized on GPIO %d (%d pixels)", pin, num_pixels);
    return strip;
}

static esp_err_t handle_rgb_set_pixel(cJSON *root, char *out, size_t sz)
{
    int pin = cJSON_GetObjectItem(root, "pin") ? cJSON_GetObjectItem(root, "pin")->valueint : -1;
    int np  = cJSON_GetObjectItem(root, "num_pixels") ? cJSON_GetObjectItem(root, "num_pixels")->valueint : 1;
    int idx = cJSON_GetObjectItem(root, "index") ? cJSON_GetObjectItem(root, "index")->valueint : 0;
    int r   = cJSON_GetObjectItem(root, "r") ? cJSON_GetObjectItem(root, "r")->valueint : 0;
    int g   = cJSON_GetObjectItem(root, "g") ? cJSON_GetObjectItem(root, "g")->valueint : 0;
    int b   = cJSON_GetObjectItem(root, "b") ? cJSON_GetObjectItem(root, "b")->valueint : 0;

    if (pin < 0) { reply_error(out, sz, "rgb_set_pixel requires pin"); return ESP_ERR_INVALID_ARG; }

    r = (r < 0) ? 0 : (r > 255 ? 255 : r);
    g = (g < 0) ? 0 : (g > 255 ? 255 : g);
    b = (b < 0) ? 0 : (b > 255 ? 255 : b);

    led_strip_handle_t strip = rgb_get_strip(pin, np, out, sz);
    if (!strip) return ESP_ERR_INVALID_STATE;

    esp_err_t err = led_strip_set_pixel(strip, (uint32_t)idx, (uint32_t)r, (uint32_t)g, (uint32_t)b);
    if (err != ESP_OK) {
        reply_error(out, sz, "led_strip_set_pixel failed: %s", esp_err_to_name(err));
        return err;
    }
    reply_ok(out, sz, NULL);
    return ESP_OK;
}

static esp_err_t handle_rgb_fill(cJSON *root, char *out, size_t sz)
{
    int pin = cJSON_GetObjectItem(root, "pin") ? cJSON_GetObjectItem(root, "pin")->valueint : -1;
    int np  = cJSON_GetObjectItem(root, "num_pixels") ? cJSON_GetObjectItem(root, "num_pixels")->valueint : 1;
    int r   = cJSON_GetObjectItem(root, "r") ? cJSON_GetObjectItem(root, "r")->valueint : 0;
    int g   = cJSON_GetObjectItem(root, "g") ? cJSON_GetObjectItem(root, "g")->valueint : 0;
    int b   = cJSON_GetObjectItem(root, "b") ? cJSON_GetObjectItem(root, "b")->valueint : 0;

    if (pin < 0) { reply_error(out, sz, "rgb_fill requires pin"); return ESP_ERR_INVALID_ARG; }

    r = (r < 0) ? 0 : (r > 255 ? 255 : r);
    g = (g < 0) ? 0 : (g > 255 ? 255 : g);
    b = (b < 0) ? 0 : (b > 255 ? 255 : b);

    led_strip_handle_t strip = rgb_get_strip(pin, np, out, sz);
    if (!strip) return ESP_ERR_INVALID_STATE;

    for (int i = 0; i < np; i++) {
        led_strip_set_pixel(strip, (uint32_t)i, (uint32_t)r, (uint32_t)g, (uint32_t)b);
    }
    reply_ok(out, sz, NULL);
    return ESP_OK;
}

static esp_err_t handle_rgb_set_range(cJSON *root, char *out, size_t sz)
{
    int pin   = cJSON_GetObjectItem(root, "pin")   ? cJSON_GetObjectItem(root, "pin")->valueint   : -1;
    int np    = cJSON_GetObjectItem(root, "num_pixels") ? cJSON_GetObjectItem(root, "num_pixels")->valueint : 1;
    int start = cJSON_GetObjectItem(root, "start") ? cJSON_GetObjectItem(root, "start")->valueint : 0;
    int end   = cJSON_GetObjectItem(root, "end")   ? cJSON_GetObjectItem(root, "end")->valueint   : 0;
    int r     = cJSON_GetObjectItem(root, "r") ? cJSON_GetObjectItem(root, "r")->valueint : 0;
    int g     = cJSON_GetObjectItem(root, "g") ? cJSON_GetObjectItem(root, "g")->valueint : 0;
    int b     = cJSON_GetObjectItem(root, "b") ? cJSON_GetObjectItem(root, "b")->valueint : 0;

    if (pin < 0) { reply_error(out, sz, "rgb_set_range requires pin"); return ESP_ERR_INVALID_ARG; }

    r = (r < 0) ? 0 : (r > 255 ? 255 : r);
    g = (g < 0) ? 0 : (g > 255 ? 255 : g);
    b = (b < 0) ? 0 : (b > 255 ? 255 : b);

    led_strip_handle_t strip = rgb_get_strip(pin, np, out, sz);
    if (!strip) return ESP_ERR_INVALID_STATE;

    if (start < 0) start = 0;
    if (end > np) end = np;
    for (int i = start; i < end; i++) {
        led_strip_set_pixel(strip, (uint32_t)i, (uint32_t)r, (uint32_t)g, (uint32_t)b);
    }
    reply_ok(out, sz, NULL);
    return ESP_OK;
}

static esp_err_t handle_rgb_show(cJSON *root, char *out, size_t sz)
{
    int pin = cJSON_GetObjectItem(root, "pin") ? cJSON_GetObjectItem(root, "pin")->valueint : -1;
    int np  = cJSON_GetObjectItem(root, "num_pixels") ? cJSON_GetObjectItem(root, "num_pixels")->valueint : 1;

    if (pin < 0) { reply_error(out, sz, "rgb_show requires pin"); return ESP_ERR_INVALID_ARG; }

    led_strip_handle_t strip = rgb_get_strip(pin, np, out, sz);
    if (!strip) return ESP_ERR_INVALID_STATE;

    /* brightness parameter (0.0 – 1.0) is accepted but not yet applied —
       the led_strip API does not expose per-show brightness scaling. */

    esp_err_t err = led_strip_refresh(strip);
    if (err != ESP_OK) {
        reply_error(out, sz, "led_strip_refresh failed: %s", esp_err_to_name(err));
        return err;
    }
    reply_ok(out, sz, NULL);
    return ESP_OK;
}

static esp_err_t handle_rgb_clear(cJSON *root, char *out, size_t sz)
{
    int pin = cJSON_GetObjectItem(root, "pin") ? cJSON_GetObjectItem(root, "pin")->valueint : -1;
    int np  = cJSON_GetObjectItem(root, "num_pixels") ? cJSON_GetObjectItem(root, "num_pixels")->valueint : 1;

    if (pin < 0) { reply_error(out, sz, "rgb_clear requires pin"); return ESP_ERR_INVALID_ARG; }

    led_strip_handle_t strip = rgb_get_strip(pin, np, out, sz);
    if (!strip) return ESP_ERR_INVALID_STATE;

    led_strip_clear(strip);
    esp_err_t err = led_strip_refresh(strip);
    if (err != ESP_OK) {
        reply_error(out, sz, "led_strip_refresh failed: %s", esp_err_to_name(err));
        return err;
    }
    reply_ok(out, sz, NULL);
    return ESP_OK;
}

/* ====================================================================
   PWM — via LEDC
   ==================================================================== */
static int pwm_find(int pin)
{
    for (int i = 0; i < s_pwm_count; i++)
        if (s_pwm[i].pin == pin && s_pwm[i].active) return i;
    return -1;
}

static esp_err_t handle_pwm_start(cJSON *root, char *out, size_t sz)
{
    int pin  = cJSON_GetObjectItem(root, "pin")  ? cJSON_GetObjectItem(root, "pin")->valueint  : -1;
    int freq = cJSON_GetObjectItem(root, "freq") ? cJSON_GetObjectItem(root, "freq")->valueint : -1;
    int duty = cJSON_GetObjectItem(root, "duty") ? cJSON_GetObjectItem(root, "duty")->valueint : -1;

    if (pin < 0 || freq <= 0 || duty < 0) {
        reply_error(out, sz, "pwm_start requires pin, freq, duty");
        return ESP_ERR_INVALID_ARG;
    }
    if (!pin_claim(pin, PIN_PWM, out, sz)) return ESP_ERR_INVALID_STATE;

    if (s_pwm_count >= MAX_PWM_CHANNELS) {
        reply_error(out, sz, "No free PWM channel (max %d)", MAX_PWM_CHANNELS);
        return ESP_ERR_NO_MEM;
    }

    ledc_channel_t ch  = (ledc_channel_t)s_pwm_count;
    ledc_timer_t   tmr = (ledc_timer_t)(s_pwm_count / 2); /* share timers */

    ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num  = tmr,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz = (uint32_t)freq,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        reply_error(out, sz, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t duty_val = ((uint32_t)duty * LEDC_DUTY_MAX) / 100;

    ledc_channel_config_t ccfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = ch,
        .timer_sel  = tmr,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = pin,
        .duty       = duty_val,
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ccfg);
    if (err != ESP_OK) {
        reply_error(out, sz, "ledc_channel_config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_pwm[s_pwm_count].pin     = pin;
    s_pwm[s_pwm_count].channel = ch;
    s_pwm[s_pwm_count].timer   = tmr;
    s_pwm[s_pwm_count].active  = true;
    s_pwm_count++;

    reply_ok(out, sz, NULL);
    return ESP_OK;
}

static esp_err_t handle_pwm_set_duty(cJSON *root, char *out, size_t sz)
{
    int pin  = cJSON_GetObjectItem(root, "pin")  ? cJSON_GetObjectItem(root, "pin")->valueint  : -1;
    int duty = cJSON_GetObjectItem(root, "duty") ? cJSON_GetObjectItem(root, "duty")->valueint : -1;
    if (pin < 0 || duty < 0) {
        reply_error(out, sz, "pwm_set_duty requires pin and duty");
        return ESP_ERR_INVALID_ARG;
    }

    int idx = pwm_find(pin);
    if (idx < 0) {
        reply_error(out, sz, "PWM not active on pin %d", pin);
        return ESP_ERR_INVALID_STATE;
    }

    uint32_t duty_val = ((uint32_t)duty * LEDC_DUTY_MAX) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, s_pwm[idx].channel, duty_val);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, s_pwm[idx].channel);

    reply_ok(out, sz, NULL);
    return ESP_OK;
}

static esp_err_t handle_pwm_set_freq(cJSON *root, char *out, size_t sz)
{
    int pin  = cJSON_GetObjectItem(root, "pin")  ? cJSON_GetObjectItem(root, "pin")->valueint  : -1;
    int freq = cJSON_GetObjectItem(root, "freq") ? cJSON_GetObjectItem(root, "freq")->valueint : -1;
    if (pin < 0 || freq <= 0) {
        reply_error(out, sz, "pwm_set_freq requires pin and freq");
        return ESP_ERR_INVALID_ARG;
    }

    int idx = pwm_find(pin);
    if (idx < 0) {
        reply_error(out, sz, "PWM not active on pin %d", pin);
        return ESP_ERR_INVALID_STATE;
    }

    ledc_set_freq(LEDC_LOW_SPEED_MODE, s_pwm[idx].timer, (uint32_t)freq);
    reply_ok(out, sz, NULL);
    return ESP_OK;
}

static esp_err_t handle_pwm_stop(cJSON *root, char *out, size_t sz)
{
    int pin = cJSON_GetObjectItem(root, "pin") ? cJSON_GetObjectItem(root, "pin")->valueint : -1;
    if (pin < 0) {
        reply_error(out, sz, "pwm_stop requires pin");
        return ESP_ERR_INVALID_ARG;
    }

    int idx = pwm_find(pin);
    if (idx < 0) {
        reply_error(out, sz, "PWM not active on pin %d", pin);
        return ESP_ERR_INVALID_STATE;
    }

    ledc_stop(LEDC_LOW_SPEED_MODE, s_pwm[idx].channel, 0);
    s_pwm[idx].active = false;
    pin_release(pin);
    reply_ok(out, sz, NULL);
    return ESP_OK;
}

/* ====================================================================
   UART
   ==================================================================== */
static int uart_get_port(int tx_pin, int rx_pin, int baud, char *out, size_t sz)
{
    /* Look for existing port with matching pins */
    for (int i = 0; i < s_uart_count; i++) {
        if (s_uart[i].tx == tx_pin && s_uart[i].rx == rx_pin)
            return (int)s_uart[i].port;
    }
    if (s_uart_count >= MAX_UART_PORTS) {
        reply_error(out, sz, "No free UART slot (max %d)", MAX_UART_PORTS);
        return -1;
    }

    if (tx_pin >= 0 && !pin_claim(tx_pin, PIN_UART, out, sz)) return -1;
    if (rx_pin >= 0 && !pin_claim(rx_pin, PIN_UART, out, sz)) return -1;

    uart_port_t port = (s_uart_count == 0) ? UART_NUM_1 : UART_NUM_2;
    uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(port, &cfg);
    if (err != ESP_OK) {
        reply_error(out, sz, "uart_param_config failed: %s", esp_err_to_name(err));
        return -1;
    }
    err = uart_set_pin(port, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        reply_error(out, sz, "uart_set_pin failed: %s", esp_err_to_name(err));
        return -1;
    }
    err = uart_driver_install(port, 512, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        reply_error(out, sz, "uart_driver_install failed: %s", esp_err_to_name(err));
        return -1;
    }

    s_uart[s_uart_count].tx   = tx_pin;
    s_uart[s_uart_count].rx   = rx_pin;
    s_uart[s_uart_count].port = port;
    s_uart_count++;
    return (int)port;
}

static esp_err_t handle_uart_write(cJSON *root, char *out, size_t sz)
{
    int tx_pin = cJSON_GetObjectItem(root, "tx") ? cJSON_GetObjectItem(root, "tx")->valueint : -1;
    cJSON *jdata = cJSON_GetObjectItem(root, "data");
    int baud = 9600;
    cJSON *jb = cJSON_GetObjectItem(root, "baud");
    if (cJSON_IsNumber(jb)) baud = jb->valueint;

    if (tx_pin < 0 || !jdata) {
        reply_error(out, sz, "uart_write requires tx and data");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buf[MAX_TRANSFER_BYTES];
    int len = parse_byte_array(jdata, buf, MAX_TRANSFER_BYTES);
    if (len <= 0) {
        reply_error(out, sz, "data must be a non-empty array of integers");
        return ESP_ERR_INVALID_ARG;
    }

    int port = uart_get_port(tx_pin, -1, baud, out, sz);
    if (port < 0) return ESP_ERR_INVALID_STATE;

    int written = uart_write_bytes((uart_port_t)port, (const char *)buf, (size_t)len);
    if (written < 0) {
        reply_error(out, sz, "uart_write_bytes failed");
        return ESP_FAIL;
    }
    reply_ok_int(out, sz, written);
    return ESP_OK;
}

static esp_err_t handle_uart_read(cJSON *root, char *out, size_t sz)
{
    int rx_pin = cJSON_GetObjectItem(root, "rx") ? cJSON_GetObjectItem(root, "rx")->valueint : -1;
    int length = cJSON_GetObjectItem(root, "length") ? cJSON_GetObjectItem(root, "length")->valueint : 64;
    int baud   = 9600;
    cJSON *jb = cJSON_GetObjectItem(root, "baud");
    if (cJSON_IsNumber(jb)) baud = jb->valueint;
    double timeout = 1.0;
    cJSON *jt = cJSON_GetObjectItem(root, "timeout");
    if (cJSON_IsNumber(jt)) timeout = jt->valuedouble;

    if (rx_pin < 0) {
        reply_error(out, sz, "uart_read requires rx");
        return ESP_ERR_INVALID_ARG;
    }
    if (length > MAX_TRANSFER_BYTES) length = MAX_TRANSFER_BYTES;

    int port = uart_get_port(-1, rx_pin, baud, out, sz);
    if (port < 0) return ESP_ERR_INVALID_STATE;

    uint8_t buf[MAX_TRANSFER_BYTES];
    TickType_t timeout_ticks = (TickType_t)((timeout * 1000.0) / portTICK_PERIOD_MS);
    if (timeout_ticks < 1) timeout_ticks = 1;
    int nread = uart_read_bytes((uart_port_t)port, buf, (uint32_t)length, timeout_ticks);
    if (nread < 0) nread = 0;

    char arr[MAX_TRANSFER_BYTES * 4 + 4];
    format_byte_array(buf, nread, arr, sizeof(arr));
    reply_ok(out, sz, arr);
    return ESP_OK;
}

/* ====================================================================
   1-Wire — software bit-bang
   ==================================================================== */
static bool onewire_reset(int pin)
{
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT_OD);
    gpio_set_level((gpio_num_t)pin, 0);
    esp_rom_delay_us(480);
    gpio_set_level((gpio_num_t)pin, 1);
    esp_rom_delay_us(70);
    bool presence = (gpio_get_level((gpio_num_t)pin) == 0);
    esp_rom_delay_us(410);
    return presence;
}

static void onewire_write_bit(int pin, int bit)
{
    gpio_set_level((gpio_num_t)pin, 0);
    if (bit) {
        esp_rom_delay_us(6);
        gpio_set_level((gpio_num_t)pin, 1);
        esp_rom_delay_us(64);
    } else {
        esp_rom_delay_us(60);
        gpio_set_level((gpio_num_t)pin, 1);
        esp_rom_delay_us(10);
    }
}

static int onewire_read_bit(int pin)
{
    gpio_set_level((gpio_num_t)pin, 0);
    esp_rom_delay_us(6);
    gpio_set_level((gpio_num_t)pin, 1);
    esp_rom_delay_us(9);
    int val = gpio_get_level((gpio_num_t)pin);
    esp_rom_delay_us(55);
    return val;
}

static void onewire_write_byte(int pin, uint8_t byte)
{
    for (int i = 0; i < 8; i++) {
        onewire_write_bit(pin, (byte >> i) & 1);
    }
}

static uint8_t onewire_read_byte(int pin)
{
    uint8_t byte = 0;
    for (int i = 0; i < 8; i++) {
        byte |= (onewire_read_bit(pin) << i);
    }
    return byte;
}

static esp_err_t handle_onewire_scan(cJSON *root, char *out, size_t sz)
{
    int pin = cJSON_GetObjectItem(root, "pin") ? cJSON_GetObjectItem(root, "pin")->valueint : -1;
    if (pin < 0) {
        reply_error(out, sz, "onewire_scan requires pin");
        return ESP_ERR_INVALID_ARG;
    }
    if (!pin_claim(pin, PIN_ONEWIRE, out, sz)) return ESP_ERR_INVALID_STATE;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    /* Simple single-device Read ROM (0x33) — works when only one device
       is on the bus.  A full Search ROM (0xF0) algorithm is not implemented. */
    if (!onewire_reset(pin)) {
        reply_ok(out, sz, "[]");
        return ESP_OK;
    }

    /* Read ROM command (only works for single device on bus) */
    onewire_write_byte(pin, 0x33);
    uint8_t rom[8];
    for (int i = 0; i < 8; i++) rom[i] = onewire_read_byte(pin);

    /* Format as array of hex strings */
    char rom_str[20];
    snprintf(rom_str, sizeof(rom_str),
             "%02X%02X%02X%02X%02X%02X%02X%02X",
             rom[0], rom[1], rom[2], rom[3],
             rom[4], rom[5], rom[6], rom[7]);

    char result[64];
    snprintf(result, sizeof(result), "[\"%s\"]", rom_str);
    reply_ok(out, sz, result);
    return ESP_OK;
}

static esp_err_t handle_onewire_read(cJSON *root, char *out, size_t sz)
{
    int pin = cJSON_GetObjectItem(root, "pin") ? cJSON_GetObjectItem(root, "pin")->valueint : -1;
    const char *rom_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "rom"));
    int command = cJSON_GetObjectItem(root, "command") ? cJSON_GetObjectItem(root, "command")->valueint : -1;

    if (pin < 0 || command < 0) {
        reply_error(out, sz, "onewire_read requires pin and command");
        return ESP_ERR_INVALID_ARG;
    }
    if (!pin_claim(pin, PIN_ONEWIRE, out, sz)) return ESP_ERR_INVALID_STATE;

    if (!onewire_reset(pin)) {
        reply_error(out, sz, "No device presence detected");
        return ESP_FAIL;
    }

    if (rom_str && strlen(rom_str) == 16) {
        /* Match ROM (0x55) then send 8-byte ROM code */
        onewire_write_byte(pin, 0x55);
        uint8_t rom[8];
        for (int i = 0; i < 8; i++) {
            char hex[3] = { rom_str[i*2], rom_str[i*2+1], '\0' };
            rom[i] = (uint8_t)strtol(hex, NULL, 16);
        }
        for (int i = 0; i < 8; i++) onewire_write_byte(pin, rom[i]);
    } else {
        /* Skip ROM (0xCC) */
        onewire_write_byte(pin, 0xCC);
    }

    onewire_write_byte(pin, (uint8_t)command);

    /* Read 9 bytes of scratchpad (standard for DS18B20) */
    uint8_t data[9];
    for (int i = 0; i < 9; i++) data[i] = onewire_read_byte(pin);

    char arr[9 * 4 + 4];
    format_byte_array(data, 9, arr, sizeof(arr));
    reply_ok(out, sz, arr);
    return ESP_OK;
}

/* ====================================================================
   Legacy RGB compat — for set_rgb backward compatibility & file persistence
   ==================================================================== */
static esp_err_t write_rgb_to_file(int r, int g, int b)
{
    FILE *file = fopen(RGB_FILE_PATH, "w");
    if (!file) {
        ESP_LOGE(TAG, "fopen %s failed, errno=%d", RGB_FILE_PATH, errno);
        return ESP_FAIL;
    }
    fprintf(file, "{\"r\":%d,\"g\":%d,\"b\":%d}", r, g, b);
    fclose(file);
    return ESP_OK;
}

/* Legacy set_rgb action — matches old tool_rgb behavior exactly */
static esp_err_t handle_set_rgb(cJSON *root, char *out, size_t sz)
{
    int r = 0, g = 0, b = 0;
    cJSON *jr = cJSON_GetObjectItem(root, "r");
    cJSON *jg = cJSON_GetObjectItem(root, "g");
    cJSON *jb = cJSON_GetObjectItem(root, "b");
    if (cJSON_IsNumber(jr)) r = jr->valueint;
    if (cJSON_IsNumber(jg)) g = jg->valueint;
    if (cJSON_IsNumber(jb)) b = jb->valueint;

    r = (r < 0) ? 0 : (r > 255 ? 255 : r);
    g = (g < 0) ? 0 : (g > 255 ? 255 : g);
    b = (b < 0) ? 0 : (b > 255 ? 255 : b);

    /* Use default onboard RGB on GPIO 48, 1 pixel */
    led_strip_handle_t strip = rgb_get_strip(48, 1, out, sz);
    if (!strip) return ESP_ERR_INVALID_STATE;

    esp_err_t err = led_strip_set_pixel(strip, 0, (uint32_t)r, (uint32_t)g, (uint32_t)b);
    if (err == ESP_OK) err = led_strip_refresh(strip);
    if (err != ESP_OK) {
        reply_error(out, sz, "RGB update failed: %s", esp_err_to_name(err));
        return err;
    }

    err = write_rgb_to_file(r, g, b);
    if (err != ESP_OK) {
        reply_error(out, sz, "Failed to write RGB values to file");
        return err;
    }

    ESP_LOGI(TAG, "RGB set to R:%d G:%d B:%d", r, g, b);
    reply_ok_str(out, sz, "RGB set");
    return ESP_OK;
}

/* ====================================================================
   Action dispatch table
   ==================================================================== */
typedef esp_err_t (*action_handler_t)(cJSON *root, char *out, size_t sz);

typedef struct {
    const char       *action;
    action_handler_t  handler;
} action_entry_t;

static const action_entry_t s_actions[] = {
    /* GPIO */
    { "gpio_set_dir",    handle_gpio_set_dir   },
    { "gpio_write",      handle_gpio_write     },
    { "gpio_read",       handle_gpio_read      },
    { "gpio_set_pull",   handle_gpio_set_pull  },
    { "gpio_on_edge",    handle_gpio_on_edge   },
    /* I2C */
    { "i2c_write",       handle_i2c_write      },
    { "i2c_read",        handle_i2c_read       },
    { "i2c_write_read",  handle_i2c_write_read },
    /* SPI */
    { "spi_transfer",    handle_spi_transfer   },
    { "spi_write",       handle_spi_write      },
    /* RGB */
    { "rgb_set_pixel",   handle_rgb_set_pixel  },
    { "rgb_fill",        handle_rgb_fill       },
    { "rgb_set_range",   handle_rgb_set_range  },
    { "rgb_show",        handle_rgb_show       },
    { "rgb_clear",       handle_rgb_clear      },
    /* PWM */
    { "pwm_start",       handle_pwm_start      },
    { "pwm_set_duty",    handle_pwm_set_duty   },
    { "pwm_set_freq",    handle_pwm_set_freq   },
    { "pwm_stop",        handle_pwm_stop       },
    /* UART */
    { "uart_write",      handle_uart_write     },
    { "uart_read",       handle_uart_read      },
    /* 1-Wire */
    { "onewire_scan",    handle_onewire_scan   },
    { "onewire_read",    handle_onewire_read   },
    /* Legacy compat */
    { "set_rgb",         handle_set_rgb        },
};

#define NUM_ACTIONS (sizeof(s_actions) / sizeof(s_actions[0]))

/* ====================================================================
   Main execute entry point
   ==================================================================== */
esp_err_t tool_gpio_execute(const char *input_json,
                            char *output,
                            size_t output_size)
{
    if (!input_json || !output) return ESP_ERR_INVALID_ARG;

    tool_gpio_init(); /* ensure initialised */

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        reply_error(output, output_size, "Invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    const char *action = cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));
    if (!action) {
        /* Backward compat: if no "action" field, treat as legacy set_rgb
           (the old tool_rgb_execute expected just r, g, b) */
        cJSON *jr = cJSON_GetObjectItem(root, "r");
        if (jr && cJSON_IsNumber(jr)) {
            esp_err_t ret = handle_set_rgb(root, output, output_size);
            cJSON_Delete(root);
            return ret;
        }
        reply_error(output, output_size, "Missing 'action' field");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < (int)NUM_ACTIONS; i++) {
        if (strcmp(action, s_actions[i].action) == 0) {
            ret = s_actions[i].handler(root, output, output_size);
            break;
        }
    }

    xSemaphoreGive(s_mutex);

    if (ret == ESP_ERR_NOT_FOUND) {
        reply_error(output, output_size, "Unknown action '%s'", action);
    }

    cJSON_Delete(root);
    return ret;
}

/* ====================================================================
   Initialization
   ==================================================================== */
esp_err_t tool_gpio_init(void)
{
    if (s_mutex != NULL) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    memset(s_pin_usage, PIN_FREE, sizeof(s_pin_usage));
    for (int i = 0; i < GPIO_PIN_MAX; i++) s_edge_count[i] = 0;
    s_i2c_count  = 0;
    s_spi_count  = 0;
    s_rgb_count  = 0;
    s_pwm_count  = 0;
    s_uart_count = 0;

    ESP_LOGI(TAG, "GPIO tool initialized");
    return ESP_OK;
}

/* Restore onboard RGB from SPIFFS (replaces read_rgb_from_file_and_apply) */
esp_err_t tool_gpio_rgb_restore(void)
{
    tool_gpio_init();

    FILE *file = fopen(RGB_FILE_PATH, "r");
    if (!file) {
        ESP_LOGW(TAG, "No saved RGB state (%s)", RGB_FILE_PATH);
        return ESP_OK;
    }

    char buf[64];
    size_t n = fread(buf, 1, sizeof(buf) - 1, file);
    fclose(file);
    buf[n] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        ESP_LOGW(TAG, "Invalid JSON in %s", RGB_FILE_PATH);
        return ESP_ERR_INVALID_ARG;
    }

    int r = 0, g = 0, b = 0;
    cJSON *jr = cJSON_GetObjectItem(json, "r");
    cJSON *jg = cJSON_GetObjectItem(json, "g");
    cJSON *jb = cJSON_GetObjectItem(json, "b");
    if (cJSON_IsNumber(jr)) r = jr->valueint;
    if (cJSON_IsNumber(jg)) g = jg->valueint;
    if (cJSON_IsNumber(jb)) b = jb->valueint;
    cJSON_Delete(json);

    char out[128];
    led_strip_handle_t strip = rgb_get_strip(48, 1, out, sizeof(out));
    if (!strip) {
        ESP_LOGE(TAG, "RGB restore: strip init failed");
        return ESP_FAIL;
    }

    esp_err_t err = led_strip_set_pixel(strip, 0, (uint32_t)r, (uint32_t)g, (uint32_t)b);
    if (err == ESP_OK) err = led_strip_refresh(strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RGB restore failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "RGB restored to R:%d G:%d B:%d", r, g, b);
    return ESP_OK;
}
