#include "mpy/mpy_gpio_module.h"

#include <string.h>
#include <stdio.h>

#include "tools/tool_gpio.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "py/runtime.h"
#include "py/objmodule.h"
#include "py/qstr.h"

static const char *TAG = "mpy_gpio_module";

/* ── Helpers ──────────────────────────────────────────────── */

/**
 * Execute a tool_gpio action via the JSON interface.
 * On failure, raises a MicroPython exception.
 */
static void gpio_json_exec(const char *json_str)
{
    char out[512];
    esp_err_t err = tool_gpio_execute(json_str, out, sizeof(out));
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT(out));
    }
}

/**
 * Execute a tool_gpio action, parse the JSON result, and return
 * the "result" field as a MicroPython object.
 */
static mp_obj_t gpio_json_exec_result(const char *json_str)
{
    char out[512];
    esp_err_t err = tool_gpio_execute(json_str, out, sizeof(out));
    if (err != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT(out));
    }
    cJSON *root = cJSON_Parse(out);
    if (!root) {
        return mp_obj_new_str(out, strlen(out));
    }
    mp_obj_t result;
    cJSON *jresult = cJSON_GetObjectItem(root, "result");
    if (cJSON_IsNumber(jresult)) {
        result = mp_obj_new_int(jresult->valueint);
    } else if (cJSON_IsString(jresult)) {
        result = mp_obj_new_str(jresult->valuestring, strlen(jresult->valuestring));
    } else {
        char *s = cJSON_PrintUnformatted(root);
        result = mp_obj_new_str(s ? s : out, strlen(s ? s : out));
        free(s);
    }
    cJSON_Delete(root);
    return result;
}

/* ── gpio module ──────────────────────────────────────────── */

/* gpio.write(pin, value) */
static mp_obj_t mod_gpio_write(mp_obj_t pin_obj, mp_obj_t val_obj)
{
    int pin = mp_obj_get_int(pin_obj);
    int val = mp_obj_get_int(val_obj);
    char json[128];
    snprintf(json, sizeof(json),
             "{\"action\":\"gpio_write\",\"pin\":%d,\"value\":%d}", pin, val);
    gpio_json_exec(json);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_gpio_write_obj, mod_gpio_write);

/* gpio.read(pin) → value */
static mp_obj_t mod_gpio_read(mp_obj_t pin_obj)
{
    int pin = mp_obj_get_int(pin_obj);
    char json[128];
    snprintf(json, sizeof(json),
             "{\"action\":\"gpio_read\",\"pin\":%d}", pin);
    return gpio_json_exec_result(json);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_gpio_read_obj, mod_gpio_read);

/* ── i2c module ───────────────────────────────────────────── */

/* i2c.write(sda, scl, addr, data, freq=100000) */
static mp_obj_t mod_i2c_write(size_t n_args, const mp_obj_t *args)
{
    int sda  = mp_obj_get_int(args[0]);
    int scl  = mp_obj_get_int(args[1]);
    int addr = mp_obj_get_int(args[2]);
    int freq = (n_args > 4) ? mp_obj_get_int(args[4]) : 100000;

    /* Build JSON data array from the Python list */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "action", "i2c_write");
    cJSON_AddNumberToObject(root, "sda", sda);
    cJSON_AddNumberToObject(root, "scl", scl);
    cJSON_AddNumberToObject(root, "addr", addr);
    cJSON_AddNumberToObject(root, "freq", freq);

    mp_obj_t data_obj = args[3];
    size_t data_len;
    mp_obj_t *data_items;
    mp_obj_get_array(data_obj, &data_len, &data_items);

    cJSON *data = cJSON_AddArrayToObject(root, "data");
    for (size_t i = 0; i < data_len; i++) {
        cJSON_AddItemToArray(data, cJSON_CreateNumber(mp_obj_get_int(data_items[i])));
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("i2c.write: out of memory"));
    }

    gpio_json_exec(json_str);
    free(json_str);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_i2c_write_obj, 4, 5, mod_i2c_write);

/* i2c.read(sda, scl, addr, len, freq=100000) → string */
static mp_obj_t mod_i2c_read(size_t n_args, const mp_obj_t *args)
{
    int sda  = mp_obj_get_int(args[0]);
    int scl  = mp_obj_get_int(args[1]);
    int addr = mp_obj_get_int(args[2]);
    int len  = mp_obj_get_int(args[3]);
    int freq = (n_args > 4) ? mp_obj_get_int(args[4]) : 100000;

    char json[256];
    snprintf(json, sizeof(json),
             "{\"action\":\"i2c_read\",\"sda\":%d,\"scl\":%d,"
             "\"addr\":%d,\"length\":%d,\"freq\":%d}",
             sda, scl, addr, len, freq);
    return gpio_json_exec_result(json);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_i2c_read_obj, 4, 5, mod_i2c_read);

/* ── spi module ───────────────────────────────────────────── */

/* spi.transfer(mosi, miso, sclk, cs, tx) → result */
static mp_obj_t mod_spi_transfer(size_t n_args, const mp_obj_t *args)
{
    int mosi = mp_obj_get_int(args[0]);
    int miso = mp_obj_get_int(args[1]);
    int sclk = mp_obj_get_int(args[2]);
    int cs   = mp_obj_get_int(args[3]);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "action", "spi_transfer");
    cJSON_AddNumberToObject(root, "mosi", mosi);
    cJSON_AddNumberToObject(root, "miso", miso);
    cJSON_AddNumberToObject(root, "sclk", sclk);
    cJSON_AddNumberToObject(root, "cs", cs);

    mp_obj_t tx_obj = args[4];
    size_t tx_len;
    mp_obj_t *tx_items;
    mp_obj_get_array(tx_obj, &tx_len, &tx_items);

    cJSON *tx = cJSON_AddArrayToObject(root, "tx");
    for (size_t i = 0; i < tx_len; i++) {
        cJSON_AddItemToArray(tx, cJSON_CreateNumber(mp_obj_get_int(tx_items[i])));
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("spi.transfer: out of memory"));
    }

    mp_obj_t result = gpio_json_exec_result(json_str);
    free(json_str);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_spi_transfer_obj, 5, 5, mod_spi_transfer);

/* ── rgb module ───────────────────────────────────────────── */

/* rgb.fill(pin, n, r, g, b) */
static mp_obj_t mod_rgb_fill(size_t n_args, const mp_obj_t *args)
{
    int pin = mp_obj_get_int(args[0]);
    int n   = mp_obj_get_int(args[1]);
    int r   = mp_obj_get_int(args[2]);
    int g   = mp_obj_get_int(args[3]);
    int b   = mp_obj_get_int(args[4]);

    char json[256];
    snprintf(json, sizeof(json),
             "{\"action\":\"rgb_fill\",\"pin\":%d,\"num_pixels\":%d,"
             "\"r\":%d,\"g\":%d,\"b\":%d}",
             pin, n, r, g, b);
    gpio_json_exec(json);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_rgb_fill_obj, 5, 5, mod_rgb_fill);

/* rgb.show(pin, n) */
static mp_obj_t mod_rgb_show(mp_obj_t pin_obj, mp_obj_t n_obj)
{
    int pin = mp_obj_get_int(pin_obj);
    int n   = mp_obj_get_int(n_obj);

    char json[128];
    snprintf(json, sizeof(json),
             "{\"action\":\"rgb_show\",\"pin\":%d,\"num_pixels\":%d}", pin, n);
    gpio_json_exec(json);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_rgb_show_obj, mod_rgb_show);

/* ── pwm module ───────────────────────────────────────────── */

/* pwm.start(pin, freq, duty) */
static mp_obj_t mod_pwm_start(mp_obj_t pin_obj, mp_obj_t freq_obj, mp_obj_t duty_obj)
{
    int pin  = mp_obj_get_int(pin_obj);
    int freq = mp_obj_get_int(freq_obj);
    int duty = mp_obj_get_int(duty_obj);

    char json[128];
    snprintf(json, sizeof(json),
             "{\"action\":\"pwm_start\",\"pin\":%d,\"freq\":%d,\"duty\":%d}",
             pin, freq, duty);
    gpio_json_exec(json);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_pwm_start_obj, mod_pwm_start);

/* ── time_ms module ───────────────────────────────────────── */

/* time_ms.sleep(ms) */
static mp_obj_t mod_time_ms_sleep(mp_obj_t ms_obj)
{
    int ms = mp_obj_get_int(ms_obj);
    vTaskDelay(pdMS_TO_TICKS(ms));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_time_ms_sleep_obj, mod_time_ms_sleep);

/* ── Registration ─────────────────────────────────────────── */

/**
 * Helper: create a module with the given name and populate its dict
 * with the provided function objects.
 */
static void register_module(qstr name, const mp_rom_map_elem_t *table, size_t count)
{
    mp_obj_t mod = mp_obj_new_module(name);
    mp_obj_dict_t *dict = mp_obj_module_get_globals(mod);
    for (size_t i = 0; i < count; i++) {
        mp_obj_dict_store(MP_OBJ_FROM_PTR(dict),
                          table[i].key,
                          table[i].value);
    }
}

void mpy_register_hw_modules(void)
{
    /* gpio module */
    {
        static const mp_rom_map_elem_t gpio_table[] = {
            { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_gpio) },
            { MP_ROM_QSTR(MP_QSTR_write),    MP_ROM_PTR(&mod_gpio_write_obj) },
            { MP_ROM_QSTR(MP_QSTR_read),     MP_ROM_PTR(&mod_gpio_read_obj) },
        };
        register_module(MP_QSTR_gpio, gpio_table, MP_ARRAY_SIZE(gpio_table));
    }

    /* i2c module */
    {
        static const mp_rom_map_elem_t i2c_table[] = {
            { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_i2c) },
            { MP_ROM_QSTR(MP_QSTR_write),    MP_ROM_PTR(&mod_i2c_write_obj) },
            { MP_ROM_QSTR(MP_QSTR_read),     MP_ROM_PTR(&mod_i2c_read_obj) },
        };
        register_module(MP_QSTR_i2c, i2c_table, MP_ARRAY_SIZE(i2c_table));
    }

    /* spi module */
    {
        static const mp_rom_map_elem_t spi_table[] = {
            { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_spi) },
            { MP_ROM_QSTR(MP_QSTR_transfer), MP_ROM_PTR(&mod_spi_transfer_obj) },
        };
        register_module(MP_QSTR_spi, spi_table, MP_ARRAY_SIZE(spi_table));
    }

    /* rgb module */
    {
        static const mp_rom_map_elem_t rgb_table[] = {
            { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_rgb) },
            { MP_ROM_QSTR(MP_QSTR_fill),     MP_ROM_PTR(&mod_rgb_fill_obj) },
            { MP_ROM_QSTR(MP_QSTR_show),     MP_ROM_PTR(&mod_rgb_show_obj) },
        };
        register_module(MP_QSTR_rgb, rgb_table, MP_ARRAY_SIZE(rgb_table));
    }

    /* pwm module */
    {
        static const mp_rom_map_elem_t pwm_table[] = {
            { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_pwm) },
            { MP_ROM_QSTR(MP_QSTR_start),    MP_ROM_PTR(&mod_pwm_start_obj) },
        };
        register_module(MP_QSTR_pwm, pwm_table, MP_ARRAY_SIZE(pwm_table));
    }

    /* time_ms module */
    {
        static const mp_rom_map_elem_t time_ms_table[] = {
            { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_time_ms) },
            { MP_ROM_QSTR(MP_QSTR_sleep),    MP_ROM_PTR(&mod_time_ms_sleep_obj) },
        };
        register_module(MP_QSTR_time_ms, time_ms_table, MP_ARRAY_SIZE(time_ms_table));
    }

    ESP_LOGI(TAG, "MicroPython hardware modules registered");
}
