# EdgeClaw

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

![EdgeClaw Banner](assets/EdgeClaw.png)

Inspired by [mimiclaw](https://github.com/memovai/mimiclaw) that enables LLM-controlled hardware interaction with ESP32 devices.

## Features

- 🔧 **Unified GPIO Tool** - Single `gpio` tool for all hardware I/O: GPIO, I²C, SPI, RGB/WS2812B, PWM, UART, 1-Wire
- 🎨 **RGB LED Control** - Control WS2812 RGB LEDs via `gpio` tool actions
- 🚀 **ArXiv Search** - Search papers in ArXiv website

## Quick Start

Clone into your mimiclaw project's components directory:
```bash
cd /path/to/your/mimiclaw/project/components
git clone https://github.com/wjc1207/mimiclaw_DLC.git
```

## Usage

**RGB LED Control:**
```c
"Turn RGB LED to a 'mys'(Swidish) color"
```

**GPIO Control:**
```c
"Set GPIO pin 4 to output and drive it HIGH"
```

**ArXiv Search:**
```c
"Search papers about Finite Rate Innovation (FRI) on ArXiV website"
``` 

## Configuration

Edit `tool_gpio.c` for default GPIO pin and LED count.

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
