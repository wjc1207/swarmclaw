# EdgeClaw

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

![EdgeClaw Banner](assets/EdgeClaw.png)

Inspired by [mimiclaw](https://github.com/memovai/mimiclaw) that enables LLM-controlled hardware interaction with ESP32 devices.

## Features

- Run OpenClaw on a ￥30 chip.

## Quick Start

```bash
# You need ESP-IDF v5.5+ installed first:
# https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/get-started/

git clone https://github.com/memovai/mimiclaw.git
cd mimiclaw

idf.py set-target esp32s3
```

Then build and flash:

```bash
# Clean build (required after any mimi_secrets.h change)
idf.py fullclean && idf.py build

# Find your serial port
ls /dev/cu.usb*          # macOS
ls /dev/ttyACM*          # Linux

# Flash and monitor (replace PORT with your port)
# USB adapter: likely /dev/cu.usbmodem11401 (macOS) or /dev/ttyACM0 (Linux)
idf.py -p PORT flash monitor
```

## Tools

| Tool | Usage | 
|----------|-------|
| **GPIO Control** | Single `gpio` tool for all hardware I/O: GPIO, I²C, SPI, RGB/WS2812B, PWM, UART, 1-Wire | 
| **Web Search** | Search anything on the Internet | 

## Supported LLM Providers

| Provider | Value | API Endpoint | Notes |
|----------|-------|-------------|-------|
| Anthropic (Claude) | `anthropic` | api.anthropic.com | Default |
| OpenAI (GPT) | `openai` | api.openai.com | |
| OpenRouter | `openrouter` | openrouter.ai | Free tier available |
| NVIDIA NIM | `nvidia` | integrate.api.nvidia.com | Free tier available |

## Supported Web Search Providers

| Provider | Value | API Endpoint | Notes |
|----------|-------|-------------|-------|
| tavily | `tavily` | api.tavily.com | Default |
| brave | `brave` | api.search.brave.com | |

## Hardware Requirements

- ESP32 development board with PSRAM
- WS2812 RGB LED (default GPIO 48)


## License

MIT License - see [LICENSE](LICENSE) file for details.

## Links

- [mimiclaw](https://github.com/memovai/mimiclaw) - Main project
- Author: Junchi Wang
