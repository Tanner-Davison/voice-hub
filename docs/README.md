# Voice Hub — Arduino Giga R1 WiFi

A local, offline-first voice command system using TinyML keyword spotting.
No cloud required. All inference runs on-device.

---

## Hardware

| Component  | Details              |
| ---------- | -------------------- |
| Board      | Arduino Giga R1 WiFi |
| Microphone | INMP441 MEMS I2S     |

### INMP441 Wiring

| INMP441 Pin | Giga R1 Pin | Notes                   |
| ----------- | ----------- | ----------------------- |
| VDD         | 3.3V        |                         |
| GND         | GND         |                         |
| SD          | Pin 35      | I2S Data                |
| WS          | Pin 25      | I2S Word Select (LRCLK) |
| SCK         | Pin 5       | I2S Bit Clock           |
| L/R         | GND         | Selects left channel    |

> ⚠️ Verify these pin numbers against the official Giga R1 pinout diagram.
> The I2S peripheral assignments may differ depending on your Arduino core version.

---

## Software Setup

### 1. Arduino Libraries (Library Manager)

- `ArduinoHttpClient`
- `ArduinoJson`
- `Arduino_AdvancedAnalog`

### 2. Train Your Model on Edge Impulse

1. Create a free account at https://edgeimpulse.com
2. Create a new project → Audio (keyword spotting)
3. Record ~50 samples per keyword (e.g. "lights on", "lights off", "good night")
4. Record ~50 samples of background noise
5. Train the model (Impulse Design → MFCC → Neural Network)
6. Export: **Deployment → Arduino Library → Build**
7. In Arduino IDE: Sketch → Include Library → Add .ZIP Library

### 3. Update ClassifierBridge.h

Uncomment and update the include at the top of `src/classifier/ClassifierBridge.h`:

```cpp
#include <voice_hub_inferencing.h>  // your exported library name
```

### 4. Update config.h

Fill in:

- WiFi SSID and password
- Webhook hosts, ports, and paths for each keyword

### 5. Upload

- Select board: **Arduino Giga R1 WiFi (M7)**
- Upload `voice_hub.ino`
- Open Serial Monitor at 115200 baud

---

## Project Structure

```
voice-hub/
├── voice_hub.ino              # Main sketch
├── src/
│   ├── config.h               # WiFi credentials & webhook targets
│   ├── audio/
│   │   └── I2SMicrophone.h    # INMP441 I2S capture
│   ├── wifi/
│   │   └── WiFiManager.h      # WiFi connection & HTTP webhooks
│   └── classifier/
│       └── ClassifierBridge.h # Edge Impulse inference wrapper
└── docs/
    └── README.md
```

---

## Adding Commands

1. Train the keyword in Edge Impulse and retrain/re-export the model
2. Add a new entry to `Config::WEBHOOKS[]` in `config.h`:

```cpp
{
    /* command */ "fan on",
    /* host    */ "homeassistant.local",
    /* port    */ 8123,
    /* path    */ "/api/webhook/fan_on",
    /* method  */ "POST",
    /* body    */ nullptr
},
```

---

## Webhook Targets

| Service        | Host                       | Notes                             |
| -------------- | -------------------------- | --------------------------------- |
| Home Assistant | `homeassistant.local:8123` | `/api/webhook/<id>`               |
| IFTTT          | `maker.ifttt.com`          | `/trigger/<event>/with/key/<key>` |
| ntfy.sh        | `ntfy.sh`                  | `/<topic>` via POST               |
| Custom server  | anything                   | Any REST endpoint                 |

## After Adding Any New Libraries Run:

```bash
arduino-cli compile \
  --fqbn arduino:mbed_giga:giga \
  --only-compilation-database \
  /home/tanner/projects/cpp/arduino/voice-hub \
  && cp /home/tanner/.cache/arduino/sketches/FCE07FC75D3BAFAA0DEC4DA1B2D55F2B/compile_commands.json \
     /home/tanner/projects/cpp/arduino/voice-hub/

```
