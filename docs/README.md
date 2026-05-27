# Voice Hub — Arduino Giga R1 WiFi

A local, offline-first voice assistant using TinyML keyword spotting, Ollama (phi3:mini), and Kokoro TTS.
No cloud required. All inference runs locally.

---

## Hardware

| Component  | Details                     | Status               |
| ---------- | --------------------------- | -------------------- |
| Board      | Arduino Giga R1 WiFi        | ✅ In hand           |
| Display    | Arduino GIGA Display Shield | ✅ In hand           |
| Microphone | INMP441 MEMS I2S            | ✅ In hand (shield)  |
| Antenna    | u.FL 2.4/5GHz flexible      | ✅ In hand           |
| Speaker    | Bluetooth speaker (A2DP)    | 🔜 Future            |

### INMP441 Wiring

| INMP441 Pin | Giga R1 Pin   | Notes                   |
| ----------- | ------------- | ----------------------- |
| VDD         | 3.3V          |                         |
| GND         | GND           |                         |
| SD          | PG_9          | I2S Data In             |
| WS          | PG_10         | I2S Word Select (LRCLK) |
| SCK         | PG_11         | I2S Bit Clock           |
| L/R         | GND           | Selects left channel    |
| MCK         | not connected | INMP441 doesn't need it |

> **Note:** The GIGA Display Shield also has a built-in digital microphone
> which can be used instead of the INMP441.

---

## Local Services

Three services must be running on your Windows PC for the full pipeline to work.
All three need to be started before testing. Keep each in its own terminal.

---

### 1. Ollama (LLM — phi3:mini)

Handles natural language questions from the board.

**Start (PowerShell):**

```powershell
$env:OLLAMA_HOST = "0.0.0.0:11434"
ollama serve
```

**Verify:**

```bash
# From WSL
curl http://10.0.0.161:11434/api/tags
```

You should see a JSON list of installed models. If it times out, check that the
Windows Firewall rule for port 11434 exists:

```powershell
New-NetFirewallRule -DisplayName "Ollama" -Direction Inbound -Protocol TCP -LocalPort 11434 -Action Allow
```

---

### 2. Kokoro TTS Server (text-to-speech)

Converts Ollama's text responses into spoken WAV audio.

**Requirements:**

- Python 3.13: `C:\Users\Tanner\AppData\Local\Programs\Python\Python313\python.exe`
- Model files in `C:\Users\Tanner\`: `kokoro-v1.0.onnx` and `voices-v1.0.bin`

**Start (PowerShell):**

```powershell
cd C:\Users\Tanner

& "C:\Users\Tanner\AppData\Local\Programs\Python\Python313\python.exe" "\\wsl$\Ubuntu\home\tanner\projects\react\voice-hub-dashboard\scripts\kokoro_server.py"
```

**Verify:**

```bash
# From WSL
curl http://10.0.0.161:8880/health
# Expected: {"ok": true}
```

If it times out, add the firewall rule:

```powershell
New-NetFirewallRule -DisplayName "Kokoro TTS" -Direction Inbound -Protocol TCP -LocalPort 8880 -Action Allow
```

---

### 3. Voice Hub Dashboard (Next.js)

The web dashboard and API bridge between the board, Ollama, and Kokoro.

**Start (WSL):**

```bash
cd ~/projects/react/voice-hub-dashboard
npm run dev
```

**Verify:** Open http://localhost:3000 in your browser.

**Requires `.env.local`** with:

```env
UPSTASH_REDIS_REST_URL=...
UPSTASH_REDIS_REST_TOKEN=...
OLLAMA_HOST=http://10.0.0.161:11434
OLLAMA_MODEL=phi3:mini
KOKORO_HOST=http://10.0.0.161:8880
KOKORO_VOICE=af_sky
```

---

## End-to-End Pipeline Test

Run these from WSL to verify the full stack is working before touching the board.

**Test Ollama:**

```bash
curl -s -X POST http://localhost:3000/api/query \
  -H "Content-Type: application/json" \
  -d '{"prompt": "What is the speed of light?"}'
# Expected: {"response": "The speed of light is approximately..."}
```

**Test Kokoro:**

```bash
curl -s -X POST http://localhost:3000/api/speak \
  -H "Content-Type: application/json" \
  -d '{"text": "Hello, I am your Voice Hub assistant."}' \
  --output /tmp/test.wav
# Expected: /tmp/test.wav is a valid WAV file (~100KB)
```

**Test full pipeline (query → speak):**

```bash
RESPONSE=$(curl -s -X POST http://localhost:3000/api/query \
  -H "Content-Type: application/json" \
  -d '{"prompt": "What is the speed of light?"}' \
  | python3 -c "import sys,json; print(json.load(sys.stdin)['response'])")

echo "Response: $RESPONSE"

curl -s -X POST http://localhost:3000/api/speak \
  -H "Content-Type: application/json" \
  -d "{\"text\": \"$RESPONSE\"}" \
  --output /tmp/response.wav
```

**Test board heartbeat:**

```bash
curl -X POST http://localhost:3000/api/status \
  -H "Content-Type: application/json" \
  -d '{"ip":"192.168.1.42","ssid":"YourNetwork"}'
# Expected: {"ok":true}  — board shows Online in dashboard
```

---

## Software Setup

### Arduino Libraries (install via arduino-cli)

```bash
arduino-cli lib install "ArduinoHttpClient" "ArduinoJson" "Arduino_AdvancedAnalog" "Arduino_GigaDisplay_GFX" "Arduino_GigaDisplayTouch"
```

### Train Your Model on Edge Impulse

1. Create a free account at https://edgeimpulse.com
2. Create a new project → Audio (keyword spotting)
3. Record ~50 samples per keyword (e.g. "lights on", "lights off", "good night")
4. Record ~50 samples of background noise
5. Train the model (Impulse Design → MFCC → Neural Network)
6. Export: **Deployment → Arduino Library → Build**
7. Install the exported `.zip`: `arduino-cli lib install --zip <file>.zip`

### Update ClassifierBridge.h

Uncomment and update the include at the top of `src/classifier/ClassifierBridge.h`:

```cpp
#include <voice_hub_inferencing.h>  // your exported library name
```

### Update config.h

Fill in:

- WiFi SSID and password
- `DASHBOARD_HOST` — your Windows IP (`10.0.0.161`) for local dev, Vercel URL for production
- Webhook hosts, ports, and paths for each keyword

---

## Compiling & Uploading (WSL2)

> The Giga R1 uses DFU mode for uploads. WSL2 needs usbipd to access USB devices.
> One-time setup steps are marked with 🔧.

### 🔧 One-time: udev rule

```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="2341", ATTR{idProduct}=="0366", MODE="0666"' | sudo tee /etc/udev/rules.d/99-arduino-giga.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

### 🔧 One-time: bind the device in usbipd (PowerShell as Administrator)

```powershell
usbipd bind --busid 1-1
```

### Keeping the Arduino visible to WSL (auto-attach)

```powershell
usbipd attach --wsl --busid 1-1 --auto-attach
```

> Only does anything when the Arduino is plugged in. Keep it running in a
> minimized terminal while doing Arduino work.

### Every upload: 4-step process

**Step 1 — Attach in PowerShell:**

```powershell
usbipd attach --wsl --busid 1-1
```

**Step 2 — Double-press reset** on the Giga R1. LED pulses = DFU mode.

**Step 3 — Compile + upload in WSL immediately after:**

```bash
arduino-cli compile --fqbn arduino:mbed_giga:giga \
  --upload --port /dev/ttyACM0 \
  /home/tanner/projects/cpp/arduino/voice-hub
```

**If you get `LIBUSB_ERROR_OTHER` or `exit status 74`:**

The USB connection dropped after the DFU reset. Re-attach and try again:

```powershell
# PowerShell
usbipd attach --wsl --busid 1-1
```

```bash
# WSL — immediately after
arduino-cli compile --fqbn arduino:mbed_giga:giga \
  --upload --port /dev/ttyACM0 \
  /home/tanner/projects/cpp/arduino/voice-hub
```

### Monitor serial output

```bash
arduino-cli monitor --port /dev/ttyACM0 --config baudrate=115200
```

---

## Regenerate compile_commands.json

```bash
arduino-cli compile \
  --fqbn arduino:mbed_giga:giga \
  --only-compilation-database \
  /home/tanner/projects/cpp/arduino/voice-hub \
  && cp /home/tanner/.cache/arduino/sketches/FCE07FC75D3BAFAA0DEC4DA1B2D55F2B/compile_commands.json \
     /home/tanner/projects/cpp/arduino/voice-hub/
```

---

## Project Structure

```
voice-hub/
├── voice-hub.ino              # Main sketch
├── compile_commands.json      # Generated by arduino-cli — clangd uses this
├── .clangd                    # Suppresses noise from library headers in Neovim
├── src/
│   ├── config.h               # WiFi credentials, dashboard host, webhook targets
│   ├── types.h                # Shared types (WebhookTarget)
│   ├── audio/
│   │   └── I2SMicrophone.h    # INMP441 I2S capture via AdvancedI2S
│   ├── wifi/
│   │   ├── WiFiManager.h      # WiFi connection & HTTP webhooks
│   │   └── Dashboard.h        # Posts events/heartbeats to Next.js dashboard
│   ├── display/
│   │   └── DisplayManager.h   # GIGA Display Shield UI (GFX-based)
│   └── classifier/
│       └── ClassifierBridge.h # Edge Impulse inference wrapper
└── docs/
    └── README.md
```

---

## Dashboard Routes

| Route          | Method | Description                              |
| -------------- | ------ | ---------------------------------------- |
| `/api/event`   | POST   | Board sends keyword detection or status  |
| `/api/status`  | POST   | Board heartbeat — keeps Online indicator |
| `/api/command` | GET    | Board polls for pending commands         |
| `/api/events`  | GET    | SSE stream — pushes events to browser    |
| `/api/query`   | POST   | Send a prompt to Ollama, get a response  |
| `/api/speak`   | POST   | Convert text to WAV via Kokoro TTS       |

---

## Adding Commands

1. Train the new keyword in Edge Impulse and re-export the model
2. Add an entry to `Config::WEBHOOKS[]` in `config.h`:

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

3. Regenerate `compile_commands.json`

---

## Webhook Targets

| Service        | Host                       | Notes                             |
| -------------- | -------------------------- | --------------------------------- |
| Home Assistant | `homeassistant.local:8123` | `/api/webhook/<id>`               |
| IFTTT          | `maker.ifttt.com`          | `/trigger/<event>/with/key/<key>` |
| ntfy.sh        | `ntfy.sh`                  | `/<topic>` via POST               |
| Custom server  | anything                   | Any REST endpoint                 |
