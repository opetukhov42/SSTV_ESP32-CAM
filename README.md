# ESP32-CAM SSTV Capsule for High Altitude Balloons (Modified)

This repository contains a highly optimized, modified sketch for the AI-Thinker ESP32-CAM. It captures an image, saves a backup to a local MicroSD card, and transmits the image as an audio signal over radio frequencies using the SSTV (Slow-Scan Television) Martin M1 protocol. 

This code is heavily optimized for High Altitude Balloon (HAB) capsules and custom circuit board integrations, featuring extreme deep-sleep power management and exact hardware timing.

## 🏆 Credits and Attribution
This project is built upon the fantastic foundational work from the original **SSTV Capsule V2 for High Altitude Balloons** project. 
* **Original Author/Project:** [Instructables: SSTV Capsule V2 for High Altitude Balloons](https://www.instructables.com/SSTV-Capsule-V2-for-High-Altitude-Balloons/)

## ✨ Key Features & Upgrades in this Version
* **1-Bit SD Card Mode:** Frees up critical pins on the ESP32-CAM for audio and PTT controls.
* **Non-Blocking SD Fallback:** If the SD card fails to mount or is corrupted mid-flight, the code will skip the local backup and force the SSTV transmission anyway, ensuring you never miss a radio ping.
* **True Deep Sleep Optimization:** Explicitly isolates the SD card pins and cuts power to the camera (OV2640) before sleeping, dropping active current from ~140mA to ~2-5mA during the 10-minute intervals.
* **Instant-TX Hardware Interrupt:** Wakes the capsule from deep sleep instantly to snap and transmit a picture via a physical pushbutton.
* **Precise Hardware Timing:** Utilizes an inline assembly insertion (`nop`) to ensure GPIO states stabilize on the circuit board perfectly before flashing the high-speed audio buffer.
* **Shared PTT & Indicator:** Consolidates the Push-To-Talk (PTT) trigger and the onboard red LED onto a single pin (GPIO 33) to stretch the limited I/O.

## ⚠️ CRITICAL: Software Requirements
Because of strict timing requirements for the audio generation `while()` loop, this code **is not compatible with ESP32 Arduino Core 3.x.x** (it will trigger Watchdog Timer panics).

To flash this code successfully, you **must** use an older ESP32 core:
1. Open Arduino IDE.
2. Go to **Tools** > **Board** > **Boards Manager**.
3. Search for `esp32`.
4. Select and install **Version 2.0.17**.

![ESP32-CAM Pinout](ESP32-CAM%20pinout.webp)

## 🔌 Pin Mapping and Wiring

| Component / Function | ESP32-CAM Pin | Notes |
| :--- | :--- | :--- |
| **Audio PWM Output** | `GPIO 13` | Freed by 1-bit SD mode. Connect to radio MIC input. |
| **PTT Relay / Red LED** | `GPIO 33` | Active `LOW`. Must be electrically isolated from the radio (use an Optocoupler like PC817 or a PNP transistor). |
| **Instant TX Button** | `GPIO 12` | Boot strapping pin! **Must have a 10kΩ pull-down resistor to GND.** Send `HIGH` (3.3V) to trigger instant wake/transmit. |
| **SD Card (1-bit mode)** | `GPIO 4, 14, 15` | Handled internally by the code. |
| **Camera Power Down** | `GPIO 32` | Handled internally to cut power during deep sleep. |

### Wiring the Audio Interface (Crucial Hardware Requirement)
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
* **R1 (100kΩ) & R2 (1kΩ)**: Voltage divider that reduces the 3.3V signal to a safe ~30mV mic-level signal.
* **C1 (10nF)**: Absorbs high-frequency PWM switching noise, passing only clean SSTV audio tones (1200Hz-2300Hz).
* **C2 (0.1µF - 1µF)**: Series coupling capacitor that blocks the radio's DC "plugin power" from colliding with the ESP32's pins.

### Wiring the PTT (Crucial Safety Note)
Standard radios often pull their PTT lines up to 5V, 8V, or higher. The ESP32 is a strict 3.3V device. **Do not connect GPIO 33 directly to your radio's PTT line.** 
Use an optocoupler to isolate the ESP32 from the radio's voltage and RF noise. Because GPIO 33 is active `LOW`, wire the optocoupler's anode to 3.3V, and its cathode to GPIO 33 (via a 330Ω resistor). When the pin goes LOW, it completes the circuit, activating both the onboard red LED and your radio simultaneously.

### Wiring the Instant-TX Button
GPIO 12 determines the flash voltage during boot. If it is held HIGH when the board powers on, the ESP32 will crash. 
* Connect one side of your pushbutton to **3.3V**.
* Connect the other side to **GPIO 12**.
* Connect a **10kΩ resistor** between **GPIO 12** and **GND**.

## 🚀 How It Works (The Loop)
1. **Wake Up:** The board boots (either powered on, woken by the 10-minute timer, or woken by the GPIO 12 button).
2. **SD Release:** RTC subsystem releases the SD pins from their low-power hold.
3. **Capture:** The camera takes a picture.
4. **Backup:** The image is saved to the MicroSD card (e.g., `/picture1.jpg`).
5. **Convert:** The JPEG is decoded to RGB565 and mapped to a 16-level SSTV transmission matrix.
6. **Transmit:** PTT is pulled LOW. An assembly `nop` loop stabilizes the board. The audio waveform is generated on GPIO 13.
7. **Sleep:** PTT is released, peripherals are isolated, the camera is powered down, and the ESP32 enters deep sleep for 10 minutes. 

## 🛠️ Modifying the Overlay Text
You can change the text written on the transmitted image by modifying these lines at the top of the sketch:
```cpp
String overlayTextTop = "PMR";    // Upper left, white text
String overlayTextBottom = "SSTV"; // Lower right, blue text