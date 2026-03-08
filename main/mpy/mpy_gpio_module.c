// main/mpy/mpy_gpio_module.c

#include "py/runtime.h"
#include "py/obj.h"
#include "tools/tool_gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Helpers ──────────────────────────────────────────────── */

static void gpio_json_exec(const char *json_str) {
    char out[512];
    esp_err_t err = tool_gpio_execute(json_str, out, sizeof(out));
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s"), out);
    }
}

static int gpio_json_exec_result_int(const char *json_str) {
    char out[512];
    esp_err_t err = tool_gpio_execute(json_str, out, sizeof(out));
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("%s"), out);
    }
    cJSON *root = cJSON_Parse(out);
    if (!root) return 0;
    cJSON *result = cJSON_GetObjectItem(root, "result");
    int val = cJSON_IsNumber(result) ? result->valueint : 0;
    cJSON_Delete(root);
    return val;
}

/* ── gpio module ──────────────────────────────────────────── */

// gpio.write(pin, value)
static mp_obj_t mpy_gpio_write(mp_obj_t pin_o, mp_obj_t val_o) {
    char json[128];
    snprintf(json, sizeof(json),
             "{\"action\":\"gpio_write\",\"pin\":%d,\"value\":%d}",
             mp_obj_get_int(pin_o), mp_obj_get_int(val_o));
    gpio_json_exec(json);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mpy_gpio_write_obj, mpy_gpio_write);

// gpio.read(pin) → int
static mp_obj_t mpy_gpio_read(mp_obj_t pin_o) {
    char json[128];
    snprintf(json, sizeof(json),
             "{\"action\":\"gpio_read\",\"pin\":%d}",
             mp_obj_get_int(pin_o));
    return mp_obj_new_int(gpio_json_exec_result_int(json));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mpy_gpio_read_obj, mpy_gpio_read);

// gpio.set_dir(pin, direction_str)
static mp_obj_t mpy_gpio_set_dir(mp_obj_t pin_o, mp_obj_t dir_o) {
    const char *dir = mp_obj_str_get_str(dir_o);
    char json[128];
    const char *direction = "OUT";
    if (strcmp(dir, "in") == 0 || strcmp(dir, "IN") == 0) {
        direction = "IN";
    }
    snprintf(json, sizeof(json),
             "{\"action\":\"gpio_set_dir\",\"pin\":%d,\"direction\":\"%s\"}",
             mp_obj_get_int(pin_o), direction);
    gpio_json_exec(json);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mpy_gpio_set_dir_obj, mpy_gpio_set_dir);

// gpio.set_pull(pin, pull_str)
static mp_obj_t mpy_gpio_set_pull(mp_obj_t pin_o, mp_obj_t pull_o) {
    const char *pull = mp_obj_str_get_str(pull_o);
    char json[128];
    snprintf(json, sizeof(json),
             "{\"action\":\"gpio_set_pull\",\"pin\":%d,\"pull\":\"%s\"}",
             mp_obj_get_int(pin_o), pull);
    gpio_json_exec(json);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mpy_gpio_set_pull_obj, mpy_gpio_set_pull);

static const mp_rom_map_elem_t gpio_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR_gpio) },
    { MP_ROM_QSTR(MP_QSTR_write),     MP_ROM_PTR(&mpy_gpio_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_read),      MP_ROM_PTR(&mpy_gpio_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_dir),   MP_ROM_PTR(&mpy_gpio_set_dir_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_pull),  MP_ROM_PTR(&mpy_gpio_set_pull_obj) },
};
static MP_DEFINE_CONST_DICT(gpio_module_globals, gpio_module_globals_table);
const mp_obj_module_t mpy_gpio_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&gpio_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_gpio, mpy_gpio_module);

/* ── i2c module ───────────────────────────────────────────── */

// i2c.write(sda, scl, addr, data_list, freq)
static mp_obj_t mpy_i2c_write(size_t n_args, const mp_obj_t *args) {
    int sda  = mp_obj_get_int(args[0]);
    int scl  = mp_obj_get_int(args[1]);
    int addr = mp_obj_get_int(args[2]);
    int freq = (n_args >= 5) ? mp_obj_get_int(args[4]) : 100000;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "action", "i2c_write");
    cJSON_AddNumberToObject(root, "sda", sda);
    cJSON_AddNumberToObject(root, "scl", scl);
    cJSON_AddNumberToObject(root, "addr", addr);
    cJSON_AddNumberToObject(root, "freq", freq);

    mp_obj_t data_obj = args[3];
    size_t len;
    mp_obj_t *items;
    mp_obj_list_get(data_obj, &len, &items);
    cJSON *data = cJSON_AddArrayToObject(root, "data");
    for (size_t i = 0; i < len; i++) {
        cJSON_AddItemToArray(data, cJSON_CreateNumber(mp_obj_get_int(items[i])));
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str) {
        gpio_json_exec(json_str);
        free(json_str);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mpy_i2c_write_obj, 4, 5, mpy_i2c_write);

// i2c.read(sda, scl, addr, length, freq)
static mp_obj_t mpy_i2c_read(size_t n_args, const mp_obj_t *args) {
    int sda  = mp_obj_get_int(args[0]);
    int scl  = mp_obj_get_int(args[1]);
    int addr = mp_obj_get_int(args[2]);
    int len  = mp_obj_get_int(args[3]);
    int freq = (n_args >= 5) ? mp_obj_get_int(args[4]) : 100000;

    char json[256];
    snprintf(json, sizeof(json),
             "{\"action\":\"i2c_read\",\"sda\":%d,\"scl\":%d,"
             "\"addr\":%d,\"length\":%d,\"freq\":%d}",
             sda, scl, addr, len, freq);
    return mp_obj_new_int(gpio_json_exec_result_int(json));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mpy_i2c_read_obj, 4, 5, mpy_i2c_read);

static const mp_rom_map_elem_t i2c_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_i2c) },
    { MP_ROM_QSTR(MP_QSTR_write),    MP_ROM_PTR(&mpy_i2c_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_read),     MP_ROM_PTR(&mpy_i2c_read_obj) },
};
static MP_DEFINE_CONST_DICT(i2c_module_globals, i2c_module_globals_table);
const mp_obj_module_t mpy_i2c_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&i2c_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_i2c, mpy_i2c_module);

/* ── spi module ───────────────────────────────────────────── */

// spi.transfer(mosi, miso, sclk, cs, tx_list)
static mp_obj_t mpy_spi_transfer(size_t n_args, const mp_obj_t *args) {
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
    size_t len;
    mp_obj_t *items;
    mp_obj_list_get(tx_obj, &len, &items);
    cJSON *tx = cJSON_AddArrayToObject(root, "tx");
    for (size_t i = 0; i < len; i++) {
        cJSON_AddItemToArray(tx, cJSON_CreateNumber(mp_obj_get_int(items[i])));
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str) {
        int result = gpio_json_exec_result_int(json_str);
        free(json_str);
        return mp_obj_new_int(result);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mpy_spi_transfer_obj, 5, 5, mpy_spi_transfer);

static const mp_rom_map_elem_t spi_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),    MP_ROM_QSTR(MP_QSTR_spi) },
    { MP_ROM_QSTR(MP_QSTR_transfer),    MP_ROM_PTR(&mpy_spi_transfer_obj) },
};
static MP_DEFINE_CONST_DICT(spi_module_globals, spi_module_globals_table);
const mp_obj_module_t mpy_spi_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&spi_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_spi, mpy_spi_module);

/* ── rgb module ───────────────────────────────────────────── */

// rgb.fill(pin, n, r, g, b)
static mp_obj_t mpy_rgb_fill(size_t n_args, const mp_obj_t *args) {
    char json[256];
    snprintf(json, sizeof(json),
             "{\"action\":\"rgb_fill\",\"pin\":%d,\"num_pixels\":%d,"
             "\"r\":%d,\"g\":%d,\"b\":%d}",
             mp_obj_get_int(args[0]), mp_obj_get_int(args[1]),
             mp_obj_get_int(args[2]), mp_obj_get_int(args[3]),
             mp_obj_get_int(args[4]));
    gpio_json_exec(json);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mpy_rgb_fill_obj, 5, 5, mpy_rgb_fill);

// rgb.show(pin, n)
static mp_obj_t mpy_rgb_show(mp_obj_t pin_o, mp_obj_t n_o) {
    char json[128];
    snprintf(json, sizeof(json),
             "{\"action\":\"rgb_show\",\"pin\":%d,\"num_pixels\":%d}",
             mp_obj_get_int(pin_o), mp_obj_get_int(n_o));
    gpio_json_exec(json);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mpy_rgb_show_obj, mpy_rgb_show);

static const mp_rom_map_elem_t rgb_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_rgb) },
    { MP_ROM_QSTR(MP_QSTR_fill),     MP_ROM_PTR(&mpy_rgb_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_show),     MP_ROM_PTR(&mpy_rgb_show_obj) },
};
static MP_DEFINE_CONST_DICT(rgb_module_globals, rgb_module_globals_table);
const mp_obj_module_t mpy_rgb_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&rgb_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_rgb, mpy_rgb_module);

/* ── pwm module ───────────────────────────────────────────── */

// pwm.start(pin, freq, duty)
static mp_obj_t mpy_pwm_start(mp_obj_t pin_o, mp_obj_t freq_o, mp_obj_t duty_o) {
    char json[128];
    snprintf(json, sizeof(json),
             "{\"action\":\"pwm_start\",\"pin\":%d,\"freq\":%d,\"duty\":%d}",
             mp_obj_get_int(pin_o), mp_obj_get_int(freq_o), mp_obj_get_int(duty_o));
    gpio_json_exec(json);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mpy_pwm_start_obj, mpy_pwm_start);

// pwm.stop(pin)
static mp_obj_t mpy_pwm_stop(mp_obj_t pin_o) {
    char json[128];
    snprintf(json, sizeof(json),
             "{\"action\":\"pwm_stop\",\"pin\":%d}",
             mp_obj_get_int(pin_o));
    gpio_json_exec(json);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mpy_pwm_stop_obj, mpy_pwm_stop);

static const mp_rom_map_elem_t pwm_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_pwm) },
    { MP_ROM_QSTR(MP_QSTR_start),    MP_ROM_PTR(&mpy_pwm_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop),     MP_ROM_PTR(&mpy_pwm_stop_obj) },
};
static MP_DEFINE_CONST_DICT(pwm_module_globals, pwm_module_globals_table);
const mp_obj_module_t mpy_pwm_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&pwm_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_pwm, mpy_pwm_module);

/* ── sleep module ─────────────────────────────────────────── */

// sleep.ms(ms)
static mp_obj_t mpy_sleep_ms(mp_obj_t ms_o) {
    vTaskDelay(pdMS_TO_TICKS(mp_obj_get_int(ms_o)));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mpy_sleep_ms_obj, mpy_sleep_ms);

static const mp_rom_map_elem_t sleep_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_sleep) },
    { MP_ROM_QSTR(MP_QSTR_ms),       MP_ROM_PTR(&mpy_sleep_ms_obj) },
};
static MP_DEFINE_CONST_DICT(sleep_module_globals, sleep_module_globals_table);
const mp_obj_module_t mpy_sleep_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&sleep_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_sleep, mpy_sleep_module);
