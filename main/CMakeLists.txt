idf_component_register(
    SRCS
        "mimi.c"
        "bus/message_bus.c"
        "wifi/wifi_manager.c"
        "telegram/telegram_bot.c"
        "llm/llm_proxy.c"
        "agent/agent_loop.c"
        "agent/context_builder.c"
        "memory/memory_store.c"
        "memory/session_mgr.c"
        "gateway/ws_server.c"
        "cli/serial_cli.c"
        "ota/ota_manager.c"
        "proxy/http_proxy.c"
        "cron/cron_service.c"
        "heartbeat/heartbeat.c"
        "tools/tool_registry.c"
        "tools/tool_cron.c"
        "tools/tool_web_search.c"
        "tools/tool_get_time.c"
        "tools/tool_files.c"
        "tools/tool_gpio.c"
        "tools/tool_http_request.c"
        "tools/tool_script.c"
        "mpy/mpy_runner.c"
        "mpy/mpy_gpio_module.c"
        "mpy/mpy_stdout.c"
        "mpy/mpy_port_io.c"
        "skills/skill_loader.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_http_client esp_http_server
        esp_https_ota esp_event json spiffs console vfs app_update esp-tls
        driver esp_timer led_strip micropython_embed
)

# Force linker to keep these symbols despite --gc-sections.
# They are referenced from inside libmicropython_embed.a but
# defined here in main.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,mp_import_stat"
    "-Wl,-u,mp_builtin_open_obj"
)
