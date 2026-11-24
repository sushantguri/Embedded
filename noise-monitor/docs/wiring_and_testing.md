# Wiring, Calibration, and Lab Testing Guide

## 1. Breadboard Wiring Checklist

1. **Power Rails**
   - Connect ESP32-S3 `3V3` and `GND` to breadboard power rails.
   - Power the **MAX4466** and **SSD1306 OLED** from `3.3V` (using 3.3 V ensures the MAX4466 DC midpoint sits at ~1.65 V, safely within the ESP32-S3 ADC input range).
2. **Analog Microphone (MAX4466)**
   - `VCC` -> `3.3V`, `GND` -> `GND`, `OUT` -> `GPIO 4`.
   - Start with the small trimmer potentiometer on the back of the MAX4466 at mid-position (~50× gain) and adjust if quiet rooms saturate or loud speech barely moves RMS.
3. **SSD1306 0.96" OLED (I²C)**
   - `VCC` -> `3.3V`, `GND` -> `GND`, `SDA` -> `GPIO 8`, `SCL` -> `GPIO 9`.
   - If the OLED is not connected yet, the firmware detects its absence at boot and continues running headless over UART.
4. **LEDs & Buzzer**
   - **Green LED:** `GPIO 5` -> `330 Ω` resistor -> LED anode (`+`), LED cathode (`-`) -> `GND`.
   - **Red LED:** `GPIO 6` -> `330 Ω` resistor -> LED anode (`+`), LED cathode (`-`) -> `GND`.
   - **Buzzer:** `GPIO 7` -> piezo buzzer `+` (or NPN transistor base via `1 kΩ` resistor if buzzer draws >20 mA), `-` -> `GND`.
   - For an **active** buzzer (beeps automatically when DC is applied), disable `Drive Buzzer with PWM Tone` in `idf.py menuconfig`.
5. **Push Button**
   - Connect one side to `GPIO 10` and the other side to `GND` (internal pull-up is enabled in software).

---

## 2. Calibrating Estimated dB SPL Against a Reference Meter

A basic analog microphone module outputs voltage proportional to acoustic pressure, not calibrated dB(A) SPL directly. To enable calibrated dB estimation:

1. Place the MAX4466 microphone and a **reference sound-level meter** (or smartphone calibration setup for academic prototyping) side by side, ~30–50 cm from a steady sound source (e.g., 1 kHz tone or pink noise from a speaker).
2. Run the firmware in default Relative mode and open `idf.py monitor`.
3. Note two values simultaneously:
   - Reference meter reading: $L_{\text{ref}}$ (e.g., `75.0 dB`)
   - Firmware UART `RMS` reading: $V_{\text{ref}}$ (e.g., `118.0 mV`)
4. Open `idf.py menuconfig` -> `Noise Pollution Monitor Configuration` -> `Noise Processing, Calibration & Thresholds`:
   - Enable `Enable Calibrated dB SPL Output` (`y`)
   - Set `Calibration Reference SPL (x10)` to `750` (for `75.0 dB`)
   - Set `Calibration Reference RMS Amplitude` to `118` (for `118 mV`)
   - Set `Warning Trigger Threshold (x10)` and `Warning Reset Threshold (x10)` to your chosen dB limits (e.g., `700` = `70.0 dB` trigger, `650` = `65.0 dB` reset).
5. Rebuild and flash (`idf.py build flash monitor`), then verify across at least three distinct volume levels (quiet, moderate, loud) against the reference meter.

---

## 3. Lab Testing Plan & Measurement Log (Section 12)

Record **actual observed measurements** during hardware testing below (do not substitute illustrative numbers):

| # | Test Case | Procedure | Expected Result | Observed Result | Pass/Fail |
| --- | --- | --- | --- | --- | --- |
| 1 | System startup | Power ESP32-S3 via USB, open serial monitor | Startup banner, all 5 FreeRTOS tasks start, Green LED ON | | |
| 2 | Quiet environment | Keep room quiet for 10 s | Low RMS (mV) and low relative %; status `NORMAL` | | |
| 3 | Louder sound | Clap or play steady audio near mic | RMS and level increase smoothly | | |
| 4 | OLED display | Observe 0.96" SSD1306 screen | Shows header, level (`% REL` or `dB`), status, threshold, bar meter | | |
| 5 | UART logging | Capture serial monitor / CSV output | Timestamped readings arrive every 500 ms with no corruption | | |
| 6 | Threshold & hysteresis | Cross trigger threshold, then dip slightly below trigger | `WARNING` activates at trigger, stays active inside hysteresis band, clears at reset | | |
| 7 | Button interrupt | Press button during `WARNING`, then again in `NORMAL` | Mutes buzzer during warning; cycles presets during normal | | |
| 8 | Queue integrity | Monitor UART logs for 60 s | All consumer tasks receive measurements; zero dropped blocks | | |
| 9 | Timing / latency | Check `Lat:` field in UART log and `WARNING` response time | Processing latency < 5 ms; end-to-end alert within one audio block (~32 ms) | | |
| 10 | Calibration check | Compare against reference SPL meter at 3 volume levels | Estimated dB tracks reference meter within prototype tolerance | | |
| 11 | Watchdog recovery | Temporarily add an infinite loop in a subscribed task | TWDT triggers panic & automatic reboot after 5 s | | |
