#include "lua/lua_gpio_lib.h"

#include <string.h>
#include <stdio.h>

#include "tools/tool_gpio.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lua.h"
#include "lauxlib.h"

static const char *TAG = "lua_gpio_lib";

/* ── Helpers ──────────────────────────────────────────────── */

/**
 * Execute a tool_gpio action via the JSON interface and push an
 * optional error string onto the Lua stack.
 * Returns the number of Lua return values pushed (0 or 1).
 */
static int gpio_json_exec(lua_State *L, const char *json_str)
{
    char out[512];
    esp_err_t err = tool_gpio_execute(json_str, out, sizeof(out));
    if (err != ESP_OK) {
        return luaL_error(L, "%s", out);
    }
    return 0;
}

/**
 * Execute a tool_gpio action, parse the JSON result, and push
 * the "result" field onto the Lua stack.  Returns 1 (the result).
 */
static int gpio_json_exec_result(lua_State *L, const char *json_str)
{
    char out[512];
    esp_err_t err = tool_gpio_execute(json_str, out, sizeof(out));
    if (err != ESP_OK) {
        return luaL_error(L, "%s", out);
    }
    /* Parse the JSON output to extract the result */
    cJSON *root = cJSON_Parse(out);
    if (!root) {
        lua_pushstring(L, out);
        return 1;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (cJSON_IsNumber(result)) {
        lua_pushinteger(L, result->valueint);
    } else if (cJSON_IsString(result)) {
        lua_pushstring(L, result->valuestring);
    } else {
        char *s = cJSON_PrintUnformatted(root);
        lua_pushstring(L, s ? s : out);
        free(s);
    }
    cJSON_Delete(root);
    return 1;
}

/* ── gpio module ──────────────────────────────────────────── */

/* gpio.write(pin, value) */
static int l_gpio_write(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    int val = (int)luaL_checkinteger(L, 2);
    char json[128];
    snprintf(json, sizeof(json),
             "{\"action\":\"gpio_write\",\"pin\":%d,\"value\":%d}", pin, val);
    return gpio_json_exec(L, json);
}

/* gpio.read(pin) → value */
static int l_gpio_read(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    char json[128];
    snprintf(json, sizeof(json),
             "{\"action\":\"gpio_read\",\"pin\":%d}", pin);
    return gpio_json_exec_result(L, json);
}

static const luaL_Reg gpio_lib[] = {
    {"write", l_gpio_write},
    {"read",  l_gpio_read},
    {NULL,    NULL}
};

/* ── i2c module ───────────────────────────────────────────── */

/* i2c.write(sda, scl, addr, data, freq) */
static int l_i2c_write(lua_State *L)
{
    int sda  = (int)luaL_checkinteger(L, 1);
    int scl  = (int)luaL_checkinteger(L, 2);
    int addr = (int)luaL_checkinteger(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);
    int freq = (int)luaL_optinteger(L, 5, 100000);

    /* Build JSON data array */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "action", "i2c_write");
    cJSON_AddNumberToObject(root, "sda", sda);
    cJSON_AddNumberToObject(root, "scl", scl);
    cJSON_AddNumberToObject(root, "addr", addr);
    cJSON_AddNumberToObject(root, "freq", freq);

    cJSON *data = cJSON_AddArrayToObject(root, "data");
    int n = (int)lua_rawlen(L, 4);
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 4, i);
        cJSON_AddItemToArray(data, cJSON_CreateNumber(lua_tointeger(L, -1)));
        lua_pop(L, 1);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return luaL_error(L, "i2c.write: out of memory");

    int ret = gpio_json_exec(L, json_str);
    free(json_str);
    return ret;
}

/* i2c.read(sda, scl, addr, len, freq) → string with JSON array */
static int l_i2c_read(lua_State *L)
{
    int sda  = (int)luaL_checkinteger(L, 1);
    int scl  = (int)luaL_checkinteger(L, 2);
    int addr = (int)luaL_checkinteger(L, 3);
    int len  = (int)luaL_checkinteger(L, 4);
    int freq = (int)luaL_optinteger(L, 5, 100000);

    char json[256];
    snprintf(json, sizeof(json),
             "{\"action\":\"i2c_read\",\"sda\":%d,\"scl\":%d,"
             "\"addr\":%d,\"length\":%d,\"freq\":%d}",
             sda, scl, addr, len, freq);
    return gpio_json_exec_result(L, json);
}

static const luaL_Reg i2c_lib[] = {
    {"write", l_i2c_write},
    {"read",  l_i2c_read},
    {NULL,    NULL}
};

/* ── spi module ───────────────────────────────────────────── */

/* spi.transfer(mosi, miso, sclk, cs, tx) → result */
static int l_spi_transfer(lua_State *L)
{
    int mosi = (int)luaL_checkinteger(L, 1);
    int miso = (int)luaL_checkinteger(L, 2);
    int sclk = (int)luaL_checkinteger(L, 3);
    int cs   = (int)luaL_checkinteger(L, 4);
    luaL_checktype(L, 5, LUA_TTABLE);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "action", "spi_transfer");
    cJSON_AddNumberToObject(root, "mosi", mosi);
    cJSON_AddNumberToObject(root, "miso", miso);
    cJSON_AddNumberToObject(root, "sclk", sclk);
    cJSON_AddNumberToObject(root, "cs", cs);

    cJSON *tx = cJSON_AddArrayToObject(root, "tx");
    int n = (int)lua_rawlen(L, 5);
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 5, i);
        cJSON_AddItemToArray(tx, cJSON_CreateNumber(lua_tointeger(L, -1)));
        lua_pop(L, 1);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) return luaL_error(L, "spi.transfer: out of memory");

    int ret = gpio_json_exec_result(L, json_str);
    free(json_str);
    return ret;
}

static const luaL_Reg spi_lib[] = {
    {"transfer", l_spi_transfer},
    {NULL,       NULL}
};

/* ── rgb module ───────────────────────────────────────────── */

/* rgb.fill(pin, n, r, g, b) */
static int l_rgb_fill(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    int n   = (int)luaL_checkinteger(L, 2);
    int r   = (int)luaL_checkinteger(L, 3);
    int g   = (int)luaL_checkinteger(L, 4);
    int b   = (int)luaL_checkinteger(L, 5);

    char json[256];
    snprintf(json, sizeof(json),
             "{\"action\":\"rgb_fill\",\"pin\":%d,\"num_pixels\":%d,"
             "\"r\":%d,\"g\":%d,\"b\":%d}",
             pin, n, r, g, b);
    return gpio_json_exec(L, json);
}

/* rgb.show(pin, n) */
static int l_rgb_show(lua_State *L)
{
    int pin = (int)luaL_checkinteger(L, 1);
    int n   = (int)luaL_checkinteger(L, 2);

    char json[128];
    snprintf(json, sizeof(json),
             "{\"action\":\"rgb_show\",\"pin\":%d,\"num_pixels\":%d}", pin, n);
    return gpio_json_exec(L, json);
}

static const luaL_Reg rgb_lib[] = {
    {"fill", l_rgb_fill},
    {"show", l_rgb_show},
    {NULL,   NULL}
};

/* ── pwm module ───────────────────────────────────────────── */

/* pwm.start(pin, freq, duty) */
static int l_pwm_start(lua_State *L)
{
    int pin  = (int)luaL_checkinteger(L, 1);
    int freq = (int)luaL_checkinteger(L, 2);
    int duty = (int)luaL_checkinteger(L, 3);

    char json[128];
    snprintf(json, sizeof(json),
             "{\"action\":\"pwm_start\",\"pin\":%d,\"freq\":%d,\"duty\":%d}",
             pin, freq, duty);
    return gpio_json_exec(L, json);
}

static const luaL_Reg pwm_lib[] = {
    {"start", l_pwm_start},
    {NULL,    NULL}
};

/* ── sleep module ─────────────────────────────────────────── */

/* sleep.ms(ms) */
static int l_sleep_ms(lua_State *L)
{
    int ms = (int)luaL_checkinteger(L, 1);
    vTaskDelay(pdMS_TO_TICKS(ms));
    return 0;
}

static const luaL_Reg sleep_lib[] = {
    {"ms",  l_sleep_ms},
    {NULL,  NULL}
};

/* ── Registration ─────────────────────────────────────────── */

void lua_open_gpio_libs(lua_State *L)
{
    luaL_newlib(L, gpio_lib);  lua_setglobal(L, "gpio");
    luaL_newlib(L, i2c_lib);   lua_setglobal(L, "i2c");
    luaL_newlib(L, spi_lib);   lua_setglobal(L, "spi");
    luaL_newlib(L, rgb_lib);   lua_setglobal(L, "rgb");
    luaL_newlib(L, pwm_lib);   lua_setglobal(L, "pwm");
    luaL_newlib(L, sleep_lib); lua_setglobal(L, "sleep");

    ESP_LOGI(TAG, "Lua hardware libraries registered");
}
