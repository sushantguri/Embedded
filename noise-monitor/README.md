# Real-Time Noise Pollution Monitoring System (ESP32-S3 + FreeRTOS)

An **Embedded C / FreeRTOS (ESP-IDF)** real-time environmental noise monitoring system built for the **ESP32-S3**. It combines ADC continuous DMA (or optional I²S digital audio), hardware ADC calibration, signal RMS & decibel estimation, hysteresis-protected visual/audible alerts, SSD1306 I²C OLED visualization, timestamped UART logging, GPIO interrupts, Task Watchdog monitoring, and Dynamic Frequency Scaling power management.

## Architecture Overview

```
                    +-----------------------+
                    | MAX4466 (ADC1 DMA) or |
                    | INMP441 (I2S Digital) |
                    +-----------+-----------+
                                |
                                v
                 [Audio Sampling Task | Core 1]
                                |
                      (raw_audio_queue)
                                |
                                v
                [Noise Processing Task | Core 1]
                 DC Removal -> RMS -> EMA ->
                 Relative % / Calibrated dB ->
                 Hysteresis Comparator
                 /              |              \
        (display_queue)   (alert_queue)   (logger_queue)
               /                |                \
              v                 v                 v
    [Display Task | C0]  [Alert Task | C1]  [Logger Task | C0]
     SSD1306 OLED I2C     Green/Red LEDs     Timestamped UART
     (i2c_mutex)          PWM/GPIO Buzzer    Text or CSV log
                          Button ISR -> Sem
```

## Source Code Organization

The project source code is organized into separate modules for easy development and maintenance:

- **`main.c`**: Initializes the ESP32-S3 and starts the application (NVS, power management, watchdog timer, peripherals).
- **`sensor.c`**: Reads sound data from the microphone (12-bit ADC Continuous DMA with hardware curve-fit calibration or digital I²S audio).
- **`processing.c`**: Calculates noise levels (DC offset removal, signal RMS, exponential moving average smoothing, relative/calibrated dB SPL scaling, and hysteresis comparator).
- **`display.c`**: Displays readings on the OLED screen (self-contained SSD1306 128x64 I²C driver with real-time level bar, alert status, and environment presets).
- **`alert.c`**: Controls LEDs and buzzer alerts (Green status LED, Red warning LED, 2.7 kHz PWM/GPIO buzzer cadence, and debounced push-button ISR).
- **`uart.c`**: Handles serial communication with the computer (timestamped tabular readings or CSV telemetry streaming).
- **`freertos.c`**: Manages multitasking and task scheduling (creates IPC queues, semaphores, mutexes, and schedules 5 pinned tasks across Cores 0 and 1).
- **`README.md`**: Contains project details, setup instructions, and usage information.

### Directory Structure

```text
noise-monitor/
├── CMakeLists.txt
├── sdkconfig.defaults
├── README.md
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild
│   ├── main.c              # Initializes the ESP32-S3 and starts the application
│   ├── sensor.c            # Reads sound data from the microphone
│   ├── processing.c        # Calculates noise levels
│   ├── display.c           # Displays readings on the OLED screen
│   ├── alert.c             # Controls LEDs and buzzer alerts
│   ├── uart.c              # Handles serial communication with the computer
│   ├── freertos.c          # Manages multitasking and task scheduling
│   ├── web_dashboard.c     # Wi-Fi (SoftAP/STA) + ESP-IDF HTTP API & ring buffer
│   ├── web/
│   │   └── index.html      # Mobile + laptop real-time line-chart dashboard (offline)
│   └── include/
│       ├── app_config.h
│       ├── app_types.h
│       ├── sensor.h
│       ├── processing.h
│       ├── display.h
│       ├── alert.h
│       ├── uart.h
│       ├── freertos_app.h
│       └── web_dashboard.h
├── dashboard/
│   └── bridge_server.py    # Companion laptop->phone LAN bridge (USB UART or demo)
├── test_host/
│   ├── Makefile            # Native host unit-test build (no hardware required)
│   └── test_noise_monitor.c # Verifies RMS, dB math, hysteresis, OLED, UART & API
└── docs/
    └── wiring_and_testing.md
```

## Default Hardware Pinout (ESP32-S3)

> Verify GPIO availability on your specific ESP32-S3 development board (e.g., Waveshare ESP32-S3 or ESP32-S3-DevKitC) before powering on. All pins are configurable via `idf.py menuconfig`.

| Component Pin | Default ESP32-S3 Pin | Notes |
| --- | --- | --- |
| MAX4466 `VCC` | `3.3V` | Do **not** exceed 3.3 V on ESP32-S3 ADC inputs |
| MAX4466 `GND` | `GND` | Common ground |
| MAX4466 `OUT` | `GPIO 4` (ADC1_CH3) | Must be an ADC1-capable pin (`GPIO 1–10`) |
| OLED `SDA` | `GPIO 8` | I²C0 SDA (0.96" SSD1306 128×64, addr `0x3C`) |
| OLED `SCL` | `GPIO 9` | I²C0 SCL (400 kHz Fast Mode) |
| Green LED | `GPIO 5` | Via 220–330 Ω current-limiting resistor |
| Red LED | `GPIO 6` | Via 220–330 Ω current-limiting resistor |
| Buzzer | `GPIO 7` | Passive (PWM 2.7 kHz default) or active via transistor driver |
| Push Button | `GPIO 10` | Active-low to `GND` (internal pull-up enabled) |

*Optional I²S Microphone (INMP441):* Select `Digital I2S Microphone` in `menuconfig` (defaults: `BCLK=GPIO 11`, `WS=GPIO 12`, `DIN=GPIO 13`, `L/R=GND`).

## Build, Flash, and Monitor (ESP-IDF v5.x)

1. Export your ESP-IDF environment (v5.2 or newer recommended):
   ```bash
   source $HOME/esp/esp-idf/export.sh
   ```
2. Navigate to the project directory and select the `esp32s3` target:
   ```bash
   cd noise-monitor
   idf.py set-target esp32s3
   ```
3. *(Optional)* Customize pins, thresholds, microphone backend, CSV logging, or calibration parameters:
   ```bash
   idf.py menuconfig
   # Navigate to: Noise Pollution Monitor Configuration
   ```
4. Build, flash, and open the serial monitor:
   ```bash
   idf.py build
   idf.py -p /dev/cu.usbmodem* flash monitor
   ```
   *(Exit monitor with `Ctrl+]`.)*

## Live Line-Data Dashboard (Phone & Laptop)

The project includes a responsive, zero-dependency real-time line-chart dashboard (`main/web/index.html`) that works on both **phones (iOS/Android)** and **laptops** with live scrolling line data, trigger/reset hysteresis reference lines, optional RMS amplitude overlay, touch/hover crosshair inspection, alert event history, remote threshold/mute controls, and CSV export.

### Option 1: Direct ESP32-S3 Wi-Fi (Phone & Laptop — No Router Needed)
Enabled by default (`CONFIG_NOISE_WEB_ENABLE=y`, SoftAP mode):
1. Flash and power the ESP32-S3.
2. On your **phone or laptop**, join Wi-Fi SSID **`NoiseMonitor-ESP32S3`** (password: `noisemonitor`).
3. Open **`http://192.168.4.1`** in your browser.
4. *(Optional)* Switch to **Station Mode** in `idf.py menuconfig` (`Noise Pollution Monitor Configuration -> Live Web Dashboard`) to join your existing lab/home Wi-Fi router and view the assigned IP printed in `idf.py monitor`.

### Option 2: Laptop USB Serial Direct (Chrome / Edge)
1. Plug the ESP32-S3 into your laptop via USB.
2. Open `main/web/index.html` in Chrome or Edge.
3. Expand **Data Source & Device Connection** and click **Connect USB Serial** (115200 baud).

### Option 3: Laptop-to-Phone LAN Bridge & Live Demo (Try Immediately!)
Use the zero-dependency Python companion server to test the dashboard right away or share a USB-connected ESP32-S3 with your phone over Wi-Fi:
```bash
# Live simulated demo (works immediately on laptop & phone, no hardware required):
python3 dashboard/bridge_server.py --demo

# Or bridge a USB-connected ESP32-S3 to your laptop & phone on the same Wi-Fi:
pip install pyserial
python3 dashboard/bridge_server.py --port /dev/cu.usbmodem14101 --baud 115200
```
Open the printed **`http://localhost:8080`** URL on your laptop and **`http://<your-laptop-ip>:8080`** on your phone!

## Run Host Unit Tests (No Hardware Required)

Verify the DC removal, RMS math, relative/dB scaling, hysteresis comparator, OLED formatting, and UART logger directly on macOS/Linux:

```bash
make -C test_host test
```

## Push Button Controls

- **During `WARNING` state:** Press button to **mute / unmute** the audible buzzer (the Red LED stays lit until noise drops below the reset threshold, at which point mute automatically clears).
- **During `NORMAL` state:** Press button to cycle through monitoring environment presets (`DEFAULT`, `QUIET/LIB`, `RESIDENT`, `INDUSTRIAL`).

## Calibration Note

By default, `CONFIG_NOISE_CALIBRATED_MODE` is **disabled** (`n`), and readings are displayed as **Relative Noise Level (`% REL`)** so raw uncalibrated readings are never misrepresented as certified dB(A) SPL measurements. See [docs/wiring_and_testing.md](docs/wiring_and_testing.md) for the step-by-step calibration procedure against a reference sound-level meter and the lab testing record table.
