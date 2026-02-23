# EdgeClaw

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

![EdgeClaw Banner](assets/EdgeClaw.png)

Inspired by [mimiclaw](https://github.com/memovai/mimiclaw) that enables LLM-controlled hardware interaction with ESP32 devices.

## Supported LLM Providers

| Provider | Value | API Endpoint | Notes |
|----------|-------|-------------|-------|
| Anthropic (Claude) | `anthropic` | api.anthropic.com | Default |
| OpenAI (GPT) | `openai` | api.openai.com | |
| OpenRouter | `openrouter` | openrouter.ai | Free tier available |
| NVIDIA NIM | `nvidia` | integrate.api.nvidia.com | Free tier available |

## Features

- 🎨 **RGB LED Control** - Control WS2812 RGB LEDs with JSON commands
- 📷 **ESP32-CAM Integration** - Capture images and return as base64-encoded data
- 🚀 **LLM-Ready** - Seamlessly integrates with the mimiclaw framework

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

**Camera Capture:**
```c
"Capture an image from the ESP32-CAM. "
```

## Configuration

Edit `tool_rgb.c` for GPIO pin and LED count.  
Edit `tool_capture.c` for camera URL and image size limits.

## Hardware Requirements

- ESP32 development board with PSRAM
- WS2812 RGB LED (default GPIO 48)
- ESP32-CAM module with HTTP server (for camera capture)

## License

MIT License - see [LICENSE](LICENSE) file for details.

## Links

- [mimiclaw](https://github.com/memovai/mimiclaw) - Main project
- Author: Junchi Wang
