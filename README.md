# ESP32-CAM High Altitude Balloon SSTV Capsule (V2)

A robust, deep-sleep optimized ESP32-CAM payload designed for High Altitude Balloons (HAB). This firmware captures images, overlays telemetry/callsigns, encodes them into Martin M1 SSTV audio, and triggers a handheld radio via PTT to transmit the image back to Earth.

Between transmissions, the system enters a true micro-amp deep sleep to preserve battery life in extreme environments.

## 🚀 Features

* **Martin M1 SSTV Encoding**: Perfectly timed 320x240 image transmission (approx. 114 seconds per image).
* **Text Overlay**: Custom telemetry or call-sign overlay directly on the image.
* **True Deep Sleep**: Completely powers down the camera, safely unmounts the SD card, and isolates specific GPIO pins to eliminate parasitic power drain.
* **SD Card Backup**: Saves every captured image locally to a MicroSD card before transmitting.
* **"Snap Now" Hardware Interrupt**: Instantly wake the payload from deep sleep to capture and transmit an image via a physical pushbutton.
* **Verbose Boot Diagnostics**: Detailed serial output differentiating between Cold Boots, Software Crashes, Watchdog resets, and Deep Sleep timers.

---

## 🛠 Hardware Requirements

* **ESP32-CAM Module** (OV2640 or OV3660) with PSRAM.
* **MicroSD Card** (Formatted to FAT32).
* **Handheld Radio** (e.g., Baofeng, Yaesu) with a VOX or PTT/MIC input.
* **Passive Components** for audio filtering and button stability (Resistors: 10kΩ, 100kΩ, 1kΩ. Capacitors: 10nF, 0.1µF).

---

## 🔌 Pin Mapping & Connections

*(Insert your `pinout.png` or `pinout.jpg` in the root of the repository to display the visual guide below)*

| ESP32-CAM Pin | Function | Description |
| --- | --- | --- |
| **GPIO 13** | SSTV Audio Out | High-speed PWM audio output. **Must use the Audio Interface Circuit below.** |
| **GPIO 33** | PTT & Indicator | Active LOW. Triggers the radio's Push-To-Talk and lights the onboard red LED. |
| **GPIO 12** | "Snap Now" Wake | Active HIGH. Wakes the board from deep sleep. **Requires a physical 10kΩ pull-down resistor.** |

---

## ⚠️ CRITICAL Hardware Interfaces

### 1. The Radio Audio Interface (GPIO 13)

You **must not** connect GPIO 13 directly to your radio's microphone input. The ESP32 outputs a 3.3V digital square wave, while a radio expects a ~30mV analog sine wave and outputs a DC bias voltage.

To prevent overdrive, RF splatter, and hardware damage, build this passive Low-Pass Filter, Attenuator, and DC Blocker between GPIO 13 and the radio:

```text
                 [Low-Pass Filter & Attenuator]       [DC Block]
                   
ESP32                                                               RADIO
GPIO 13  ----[ R1: 100kΩ ]----+-----------------------||------------> MIC IN
                              |                   C2: 0.1µF
                              |                       or 1µF
                         [ R2: 1kΩ ]
                              |
                         [ C1: 10nF ] 
                              |
                             GND

```

* **R1 (100kΩ) & R2 (1kΩ)**: Voltage divider that reduces the screaming 3.3V signal to a safe ~30mV mic-level signal.
* **C1 (10nF)**: Absorbs high-frequency PWM switching noise, passing only clean SSTV audio tones (1200Hz-2300Hz).
* **C2 (0.1µF - 1µF)**: Series coupling capacitor that blocks the radio's DC "plugin power" from colliding with the ESP32's pins.

### 2. The "Snap Now" Button (GPIO 12)

GPIO 12 is a flash memory strapping pin. If it is floating or held HIGH during a Cold Boot, the ESP32 will set its flash voltage to 1.8V (instead of 3.3V) and suffer an unrecoverable `Flash read err`.

* **Do NOT use software pull-downs on GPIO 12.**
* You **must** wire a physical 10kΩ pull-down resistor between GPIO 12 and GND.
* Wire your pushbutton between 3.3V and GPIO 12.
* **Never press the button while applying main power or hitting the EN/RESET button.** Only press it when the board is actively in Deep Sleep.

---

## 💻 Software Setup

1. **ESP32 Core**: This code is optimized for ESP32 Arduino Core **v2.0.x** (e.g., 2.0.17).
2. **Board Configuration** (Arduino IDE):
* **Board**: AI Thinker ESP32-CAM
* **PSRAM**: Enabled (Required for Martin M1 buffer allocation)
* **Partition Scheme**: Huge APP (3MB No OTA / 1MB SPIFFS)


3. **Configuration Variables** (in `esp32cam_sstv_ea2ept_v5.ino`):
* `TIME_TO_SLEEP`: Set the deep sleep interval in seconds (Default: `600` / 10 minutes).
* `overlayTextTop`: Top-left telemetry/callsign string.
* `overlayTextBottom`: Bottom-right telemetry/callsign string.



---

## 🔄 Operation Flow

1. **Boot**: The board wakes up, initializes the camera (QVGA 320x240 for strict Martin M1 compliance), mounts the SD card, and allocates the PSRAM frame buffer.
2. **Capture**: A photo is taken.
3. **SD Backup**: The raw JPEG is saved to the MicroSD card (e.g., `/picture1.jpg`).
4. **Processing**: The image is decompressed to an RGB888 buffer, and the text overlay is rasterized onto the pixels. An ASCII preview is dumped to the Serial monitor.
5. **Transmission**:
* GPIO 33 goes LOW, triggering the radio PTT and lighting the red indicator LED.
* A brief hardware stabilization delay occurs.
* The Martin M1 SSTV audio sequence is generated on GPIO 13.


6. **Sleep**:
* PTT is released.
* The SD card is gracefully unmounted (`SD_MMC.end()`) to prevent the `0x107` timeout glitch on the next wake.
* Camera hardware regulators are powered down.
* Unused GPIOs (Flash LED, Speaker) are isolated.
* The board enters Deep Sleep.



---

## 🩺 Troubleshooting

* **Serial monitor shows `rst:0xc (SW_CPU_RESET)` looping over and over:**
Your GPIO 12 pin is floating. The board is trying to go to sleep, but the floating pin makes it think the wake button is being held down. Install the physical 10kΩ pull-down resistor.
* **Serial monitor freezes immediately after `[INFO] ESP32-CAM SSTV Capsule Booting`:**
GPIO 12 was HIGH or pulled LOW via software during boot, causing a flash memory voltage crash. Remove any software pull-downs and ensure the pin is physically pulled to GND.
* **SD Card Error `0x107` (Timeout) upon waking from sleep:**
Ensure you are NOT isolating `GPIO_NUM_14` or `GPIO_NUM_15` during deep sleep. Isolating these pins causes phantom clock pulses that crash the SD card controller. Make sure `SD_MMC.end();` is called before sleeping.
* **Transmitted image is diagonal, slanted, or static:**
Ensure your camera `frame_size` is explicitly set to `FRAMESIZE_QVGA`. Martin M1 requires exactly 320x240 pixels.