#include <driver/ledc.h>
#include "camera.h"
#include "sin256.h"
#include "esp_crc.h"           // ESP32 built-in CRC library
#include "FS.h"                // SD Card ESP32
#include "SD_MMC.h"            // SD Card ESP32
#include <EEPROM.h>            // read and write from flash memory
#include "soc/soc.h"           // Disable brownout problems
#include "soc/rtc_cntl_reg.h"  // Disable brownout problems
#include "driver/rtc_io.h"

// define the number of bytes you want to access
#define EEPROM_SIZE 1

#define BELL202_BAUD 1200
#define F_SAMPLE ((BELL202_BAUD * 32) * 0.93) // ≈ 35712 Hz
#define FTOFTW (4294967295 / F_SAMPLE)
#define TIME_PER_SAMPLE (1000.0/F_SAMPLE)
#define uS_TO_S_FACTOR 1000000

// Updated to 600 seconds (10 minutes) for true deep sleep handling
#define TIME_TO_SLEEP  600 

#define led_flash 4
#define speaker_output 13 // Moved to 13 (freed by 1-bit SD mode)
#define ptt_pin 33        // Doubles as PTT and onboard red LED (Active LOW)

int pictureNumber = 0;

// 16-level density map (Darkest to Lightest)
const char ascii_map[] = " .:-=+*#%@MW8&B";

String overlayTextTop = "PMR";    // Upper left, white
String overlayTextBottom = "SSTV"; // Lower right, black

void print_wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0: Serial.println("[INFO] Wake Reason: External signal (Snap Now Pushbutton)"); break;
    case ESP_SLEEP_WAKEUP_EXT1: Serial.println("[INFO] Wake Reason: External signal using RTC_CNTL"); break;
    case ESP_SLEEP_WAKEUP_TIMER: Serial.println("[INFO] Wake Reason: Timer (10 Minute Interval)"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: Serial.println("[INFO] Wake Reason: Touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP: Serial.println("[INFO] Wake Reason: ULP program"); break;
    default: Serial.printf("[INFO] Wake Reason: Not caused by deep sleep (Power-on or Reset)\n"); break;
  }
}

RTC_DATA_ATTR int bootCount = 0;

volatile uint32_t FTW = FTOFTW * 1000;
volatile uint32_t PCW = 0;
volatile uint32_t TFLAG = 0;

#define FT_1000 (uint32_t) (1000 * FTOFTW)
#define FT_1100 (uint32_t) (1100 * FTOFTW)
#define FT_1200 (uint32_t) (1200 * FTOFTW)
#define FT_1300 (uint32_t) (1300 * FTOFTW)
#define FT_1500 (uint32_t) (1500 * FTOFTW)
#define FT_1900 (uint32_t) (1900 * FTOFTW)
#define FT_2200 (uint32_t) (2200 * FTOFTW)
#define FT_2300 (uint32_t) (2300 * FTOFTW)
#define FT_SYNC (uint32_t) (FT_1200)

#define MAX_WIDTH 320
#define MAX_HEIGHT 256

class SSTV_config_t {
  public:
    uint8_t vis_code;
    uint32_t width;
    uint32_t height;
    float line_time;
    float h_sync_time;
    float v_sync_time;
    float c_sync_time;
    float left_margin_time;
    float visible_pixels_time;
    float pixel_time;
    bool color;
    bool martin;
    bool robot;

    SSTV_config_t(uint8_t v) {
      vis_code = v;
      switch (vis_code) {
        case 2: // Robot B&W8
          robot = true; martin = false; color = false;
          width = 160; height = 120;
          line_time = 67.025; h_sync_time = 30.0; v_sync_time = 6.5;
          left_margin_time = 1.6;
          visible_pixels_time = line_time - v_sync_time - left_margin_time;
          pixel_time = visible_pixels_time / width;
          break;
        case 44: // Martin M1
          robot = false; martin = true; color = true;
          width = 320; height = 240;
          line_time = 446.4460001; h_sync_time = 30.0; v_sync_time = 4.862;
          c_sync_time = 0.572; left_margin_time = 0.0;
          visible_pixels_time = line_time - v_sync_time - left_margin_time - (3 * c_sync_time);
          pixel_time = visible_pixels_time / (width * 3);
          break;
      }
    }
};

camera_fb_t* fb;
SSTV_config_t* currentSSTV;

volatile uint16_t rasterX = 0;
volatile uint16_t rasterY = 0;
volatile uint8_t SSTVseq = 0;
double SSTVtime = 0;
double SSTVnext = 0;
uint8_t VISsr = 0;
uint8_t VISparity;
uint8_t HEADERptr = 0;
static uint32_t SSTV_HEADER[] = {FT_2300, 100, FT_1500, 100, FT_2300, 100, FT_1500, 100, FT_1900, 300, FT_1200, 10, FT_1900, 300, FT_1200, 30, 0, 0};
uint8_t SSTV_RUNNING = 0;

uint8_t *rgb_buf = NULL;
uint32_t rgb_buf_len;
uint16_t rgb_width;
uint16_t rgb_height;

TaskHandle_t sampleHandlerHandle;

void IRAM_ATTR audioISR() {
  PCW += FTW;
  TFLAG = 1;
}

void sampleHandler(void *p) {
  disableCore0WDT();
  while (1) {
    if (TFLAG) {
      TFLAG = 0;
      int v = SinTableH[((uint8_t*)&PCW)[3]];
      
      // ESP32 Core 2.x API for setting LEDC duty cycle
      ledc_set_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_3, v);
      ledc_update_duty(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_3);
      
      SSTVtime += TIME_PER_SAMPLE;
      if (!SSTV_RUNNING || SSTVtime < SSTVnext) continue;

      switch (SSTVseq) {
        case 0: // Start
          SSTVtime = 0; HEADERptr = 0; VISparity = 0; VISsr = currentSSTV->vis_code;
          FTW = SSTV_HEADER[HEADERptr++]; SSTVnext = (float)SSTV_HEADER[HEADERptr++];
          SSTVseq++; break;
        case 1: // VIS header
          if (SSTV_HEADER[HEADERptr + 1] == 0) {
            SSTVseq++; HEADERptr = 0;
          } else {
            FTW = SSTV_HEADER[HEADERptr++]; SSTVnext += (float)SSTV_HEADER[HEADERptr++];
          }
          break;
        case 2: // VIS code
          if (HEADERptr == 7) {
            HEADERptr = 0;
            FTW = VISparity ? FT_1100 : FT_1300;
            SSTVnext += 30.0; SSTVseq++;
          } else {
            FTW = (VISsr & 0x01) ? (VISparity ^= 0x01, FT_1100) : FT_1300;
            VISsr >>= 1; SSTVnext += 30.0; HEADERptr++;
          }
          break;
        case 3: // VIS stop bit/sync0
          FTW = FT_1200; SSTVnext += 30.0 + currentSSTV->h_sync_time;
          rasterX = 0; rasterY = 0; SSTVseq = 10; break;
        case 10: // Start of line Green
          if (rasterX == currentSSTV->width) {
            rasterX = 0; FTW = FT_1500; SSTVnext += currentSSTV->c_sync_time; SSTVseq++;
          } else {
            int G = rgb_buf[1 + (rasterX * 3) + (rasterY * currentSSTV->width * 3)];
            int f = map(G, 0, 255, 1500, 2300); FTW = FTOFTW * f;
            SSTVnext += currentSSTV->pixel_time; rasterX++;
          }
          break;
        case 11: // Blue
          if (rasterX == currentSSTV->width) {
            rasterX = 0; FTW = FT_1500; SSTVnext += currentSSTV->c_sync_time; SSTVseq++;
          } else {
            int B = rgb_buf[(rasterX * 3) + (rasterY * currentSSTV->width * 3)];
            int f = map(B, 0, 255, 1500, 2300); FTW = FTOFTW * f;
            SSTVnext += currentSSTV->pixel_time; rasterX++;
          }
          break;
        case 12: // Red
          if (rasterX == currentSSTV->width) {
            rasterX = 0; rasterY++;
            if (rasterY == currentSSTV->height) {
              SSTV_RUNNING = false; SSTVseq = 0; FTW = 0; PCW = 0;
            } else {
              FTW = FT_SYNC; SSTVnext += currentSSTV->v_sync_time; SSTVseq = 10;
            }
          } else {
            int R = rgb_buf[2 + (rasterX * 3) + (rasterY * currentSSTV->width * 3)];
            int f = map(R, 0, 255, 1500, 2300); FTW = FTOFTW * f;
            SSTVnext += currentSSTV->pixel_time; rasterX++;
          }
          break;
      }
    }
  }
}

void drawText(uint8_t *rgb_buf, uint16_t width, uint16_t height, const char *text_top, const char *text_bottom) {
  if (!rgb_buf) return;

  int char_width = 10;
  int char_height = 14;
  int spacing = 2;

  const uint8_t font[36][7] = {
    {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}, // A
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}, // B
    {0b01111, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b01111}, // C
    {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}, // D
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}, // E
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}, // F
    {0b01111, 0b10000, 0b10000, 0b10011, 0b10001, 0b10001, 0b01111}, // G
    {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}, // H
    {0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}, // I
    {0b00001, 0b00001, 0b00001, 0b00001, 0b00001, 0b10001, 0b01110}, // J
    {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}, // K
    {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}, // L
    {0b10001, 0b11011, 0b10101, 0b10001, 0b10001, 0b10001, 0b10001}, // M
    {0b10001, 0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001}, // N
    {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}, // O
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}, // P
    {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}, // Q
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}, // R
    {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}, // S
    {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}, // T
    {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}, // U
    {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}, // V
    {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b10101, 0b01010}, // W
    {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}, // X
    {0b10001, 0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100}, // Y
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}, // Z
    {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}, // 0
    {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}, // 1
    {0b01110, 0b10001, 0b00001, 0b00110, 0b01000, 0b10000, 0b11111}, // 2
    {0b01110, 0b10001, 0b00001, 0b00110, 0b00001, 0b10001, 0b01110}, // 3
    {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}, // 4
    {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}, // 5
    {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}, // 6
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}, // 7
    {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}, // 8
    {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}  // 9
  };

  int x_start_top = 10;
  int y_start_top = 10;
  int text_top_len = strlen(text_top);
  uint8_t top_color_r = 255, top_color_g = 0, top_color_b = 0;

  for (int c = 0; c < text_top_len; c++) {
    int char_index = -1;
    if (text_top[c] >= 'A' && text_top[c] <= 'Z') char_index = text_top[c] - 'A';
    else if (text_top[c] >= '0' && text_top[c] <= '9') char_index = (text_top[c] - '0') + 26;
    if (char_index >= 0) {
      for (int y = 0; y < 7; y++) {
        for (int x = 0; x < 5; x++) {
          if (font[char_index][y] & (1 << (4 - x))) {
            for (int dy = 0; dy < 2; dy++) {
              for (int dx = 0; dx < 2; dx++) {
                int pixel_x = x_start_top + (c * (char_width + spacing)) + (x * 2) + dx;
                int pixel_y = y_start_top + (y * 2) + dy;
                if (pixel_x < width && pixel_y < height) {
                  int index = (pixel_y * width + pixel_x) * 3;
                  rgb_buf[index + 0] = top_color_r;
                  rgb_buf[index + 1] = top_color_g;
                  rgb_buf[index + 2] = top_color_b;
                }
              }
            }
          }
        }
      }
    }
  }

  int text_bottom_len = strlen(text_bottom);
  int x_start_bottom = width - (text_bottom_len * (char_width + spacing)) - 10;
  int y_start_bottom = height - char_height - 10;
  uint8_t bottom_color_r = 0, bottom_color_g = 0, bottom_color_b = 255;

  for (int c = 0; c < text_bottom_len; c++) {
    int char_index = -1;
    if (text_bottom[c] >= 'A' && text_bottom[c] <= 'Z') char_index = text_bottom[c] - 'A';
    else if (text_bottom[c] >= '0' && text_bottom[c] <= '9') char_index = (text_bottom[c] - '0') + 26;
    if (char_index >= 0) {
      for (int y = 0; y < 7; y++) {
        for (int x = 0; x < 5; x++) {
          if (font[char_index][y] & (1 << (4 - x))) {
            for (int dy = 0; dy < 2; dy++) {
              for (int dx = 0; dx < 2; dx++) {
                int pixel_x = x_start_bottom + (c * (char_width + spacing)) + (x * 2) + dx;
                int pixel_y = y_start_bottom + (y * 2) + dy;
                if (pixel_x < width && pixel_y < height && pixel_x >= 0) {
                  int index = (pixel_y * width + pixel_x) * 3;
                  rgb_buf[index + 0] = bottom_color_r;
                  rgb_buf[index + 1] = bottom_color_g;
                  rgb_buf[index + 2] = bottom_color_b;
                }
              }
            }
          }
        }
      }
    }
  }
}

char get_ascii_char(uint8_t r, uint8_t g, uint8_t b) {
    // 1. Calculate weighted brightness
    uint8_t gray = (uint8_t)((r * 299 + g * 587 + b * 114) / 1000);
    // 2. Map 0-255 to 0-15
    uint8_t level = gray / 16;
    return ascii_map[level];
}

// Print outframe to Serial in ASCII
void app_printframe(uint8_t * frame, int width, int height) {
  for( int y=0; y<height; y++ ) {
    Serial.printf("%3d: ",y);
    for( int x=0; x<width; x++ ) {
      // Mirror?
      int yy= y; 
      int xx= (width-1)-x;
      // Assuming 24-bit RGB888 (3 bytes per pixel)
      uint8_t r = frame[(yy * width + xx) * 3];
      uint8_t g = frame[(yy * width + xx) * 3 + 1];
      uint8_t b = frame[(yy * width + xx) * 3 + 2];
      // Print
      Serial.printf("%c",get_ascii_char(r, g, b));
    }
    Serial.printf("\n");
  }
  Serial.printf("\n");
}

void doImage() {
  camera_fb_t *fb = NULL;
  Serial.println("\n[INFO] Starting image capture process...");
  delay(1000); 
  fb = esp_camera_fb_get();
  delay(1000);
  if (!fb) {
    Serial.println("[ERROR] Camera capture failed! Aborting transmission cycle.");
    return;
  }
  Serial.println("[OK] Image captured successfully");

  // Handle SD Card writing safely without crashing
  if (SD_MMC.cardType() != CARD_NONE) {
    pictureNumber = EEPROM.read(0) + 1;
    String path = "/picture" + String(pictureNumber) +".jpg";
    fs::FS &fs = SD_MMC; 
    Serial.printf("[INFO] Saving picture to SD Card: %s\n", path.c_str());

    File file = fs.open(path.c_str(), FILE_WRITE);
    if(!file){
      Serial.println("[ERROR] Failed to open file in writing mode on SD Card");
    } else {
      file.write(fb->buf, fb->len); // payload (image), payload length
      Serial.printf("[OK] File saved successfully\n");
      EEPROM.write(0, pictureNumber);
      EEPROM.commit();
    }
    file.close();
  } else {
    Serial.println("[WARNING] Skipping SD card write - no valid card detected.");
  }
  
  rgb_width = fb->width;
  rgb_height = fb->height;
  rgb_buf_len = rgb_width * rgb_height * 3 * sizeof(uint8_t);
  Serial.printf("[INFO] Processed RGB Buffer Size: %ix%i*3=%i bytes\n", rgb_width, rgb_height, rgb_buf_len);

  if (!rgb_buf) {
    rgb_buf = (uint8_t *) ps_malloc(rgb_buf_len);
  }
  if (!rgb_buf) {
    Serial.println("[ERROR] RGB buffer allocation failed (Out of Memory)");
    esp_camera_fb_return(fb);
    return;
  }
  Serial.println("[OK] RGB buffer allocated");

  // Calculate CRC32 of the framebuffer
  uint32_t image_crc = esp_crc32_le(0, fb->buf, fb->len);
  Serial.printf("[INFO] Original Image Length: %d bytes\n", fb->len);
  Serial.printf("[INFO] Original Image CRC32: %08X\n", image_crc);

  fmt2rgb888(fb->buf, fb->len, fb->format, rgb_buf);

  // Calculate CRC32 of the processed buffer
  uint32_t rgb888_crc = esp_crc32_le(0, rgb_buf, rgb_buf_len);
  Serial.printf("[INFO] Processed Image CRC32: %08X\n", rgb888_crc);
  
  Serial.println("[INFO] Drawing overlay text on image...");
  drawText(rgb_buf, rgb_width, rgb_height, overlayTextTop.c_str(), overlayTextBottom.c_str());

  Serial.println("\n[INFO] ASCII Preview:");
  app_printframe(rgb_buf,rgb_width,rgb_height);

  if (currentSSTV) delete currentSSTV;
  currentSSTV = new SSTV_config_t(44);
  
  Serial.println("\n===================================");
  Serial.println("[INFO] INITIATING RADIO TRANSMISSION");
  Serial.println("===================================");
  
  // Activate PTT and Red LED Indicator (LOW = GND)
  Serial.println("[OK] Activating PTT and Red LED Indicator");
  digitalWrite(ptt_pin, LOW);
  
  // Inline assembly insertion to ensure hardware stabilization before starting output
  asm volatile (
    "nop \n\t"
    "nop \n\t"
    "nop \n\t"
  );

  Serial.print("[INFO] Sending SSTV Audio ");
  SSTVtime = 0; SSTVnext = 0; SSTVseq = 0; SSTV_RUNNING = true;
  vTaskResume(sampleHandlerHandle);
  while (SSTV_RUNNING) {
    Serial.print(".");
    delay(1000); 
  }
  vTaskSuspend(sampleHandlerHandle);
  Serial.println("\n[OK] SSTV Transmission Complete");

  // Deactivate PTT and Red LED (HIGH = inactive)
  Serial.println("[OK] Deactivating PTT and Red LED");
  digitalWrite(ptt_pin, HIGH);
  
  esp_camera_fb_return(fb);
  digitalWrite(speaker_output, LOW);
  delay(10000); 

  // Liberar el buffer después de usarlo
  free(rgb_buf);
  rgb_buf = NULL;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n\n===================================");
  Serial.println("[INFO] ESP32-CAM SSTV Capsule Booting");
  Serial.println("===================================");

  // Release the pins from the RTC subsystem after waking up from deep sleep
  // This allows the SD card to reinitialize correctly
  rtc_gpio_deinit(GPIO_NUM_4);
  rtc_gpio_deinit(GPIO_NUM_13);
  rtc_gpio_deinit(GPIO_NUM_14);
  rtc_gpio_deinit(GPIO_NUM_15);
  Serial.println("[OK] RTC GPIO Pins Released");

  ++bootCount;
  Serial.println("[INFO] Boot number: " + String(bootCount));
  print_wakeup_reason();

  // Handle standard deep sleep interval
  esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR);
  // Enable external wakeup on GPIO 12 (HIGH state)
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_12, 1);
  Serial.println("[OK] Deep Sleep Timers & Wake Pins Configured (" + String(TIME_TO_SLEEP) + " Seconds)");
  
  delay(500);

  Serial.println("[INFO] Initializing Camera...");
  setupCamera();
  Serial.println("[OK] Camera Setup Complete");

  rtc_gpio_hold_dis(GPIO_NUM_4);
  
  Serial.println("[INFO] Initializing SD Card in 1-bit mode...");
  // SAFELY MOUNT SD CARD WITHOUT FATAL RETURNS
  if(!SD_MMC.begin("/sdcard", true)){
    Serial.println("[ERROR] SD Card Mount Failed - Proceeding without saving backup!");
  } else {
    Serial.println("[OK] SD Card Mounted Successfully");
    uint8_t cardType = SD_MMC.cardType();
    if(cardType == CARD_NONE){
      Serial.println("[WARNING] No SD Card media attached - Proceeding without saving backup!");
    } else {
      Serial.print("[OK] SD Card Type: ");
      if(cardType == CARD_MMC){ Serial.println("MMC"); }
      else if(cardType == CARD_SD){ Serial.println("SDSC"); }
      else if(cardType == CARD_SDHC){ Serial.println("SDHC"); }
      else { Serial.println("UNKNOWN"); }
      
      uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
      Serial.printf("[OK] SD Card Size: %lluMB\n", cardSize);
    }
  }

  Serial.println("[INFO] Initializing EEPROM...");
  EEPROM.begin(EEPROM_SIZE);
  Serial.println("[OK] EEPROM Ready");
  
  Serial.println("[INFO] Configuring PTT and LED pins...");
  // Set up the shared PTT and LED pin
  pinMode(ptt_pin, OUTPUT); 
  digitalWrite(ptt_pin, HIGH); // PTT inactive (and red LED OFF) at startup
  Serial.println("[OK] Hardware Pins Configured");

  Serial.println("[INFO] Initializing SSTV Audio Timers & PWM...");
  // Core 2.x Original Timer Implementation
  hw_timer_t *timer = NULL;
  timer = timerBegin(2, 10, true);
  timerAttachInterrupt(timer, &audioISR, true);
  timerAlarmWrite(timer, 8000000 / F_SAMPLE, true);
  timerAlarmEnable(timer);

  // Core 2.x Original LEDC implementation
  ledc_timer_config_t ledc_timer;
  ledc_timer.speed_mode = LEDC_HIGH_SPEED_MODE;
  ledc_timer.duty_resolution = LEDC_TIMER_8_BIT;
  ledc_timer.timer_num = LEDC_TIMER_1;
  ledc_timer.freq_hz = 200000;
  ledc_timer_config(&ledc_timer);

  ledc_channel_config_t ledc_channel;
  ledc_channel.channel = LEDC_CHANNEL_3;
  ledc_channel.gpio_num = speaker_output;
  ledc_channel.speed_mode = LEDC_HIGH_SPEED_MODE;
  ledc_channel.timer_sel = LEDC_TIMER_1;
  ledc_channel.duty = 2;
  ledc_channel.hpoint = 0;
  ledc_channel_config(&ledc_channel);
  Serial.println("[OK] Audio Hardware Ready");

  xTaskCreatePinnedToCore(sampleHandler, "IN", 4096, NULL, 1, &sampleHandlerHandle, 0);
  vTaskSuspend(sampleHandlerHandle);
  
  Serial.println("[INFO] Setup complete. Entering transmission phase...");
}

void loop() {
  doImage();
  Serial.println("\n[INFO] Cycle complete. Preparing for deep sleep...");
  
  // 1. Turn off the camera to save battery
  pinMode(32, OUTPUT);
  digitalWrite(32, HIGH); 
  
  // 2. Isolate unused/leaky pins (Skipping GPIO 12 so the wakeup button still functions)
  rtc_gpio_isolate(GPIO_NUM_4);  // Flash LED
  rtc_gpio_isolate(GPIO_NUM_13); // Speaker output
  rtc_gpio_isolate(GPIO_NUM_14); // SD Clock
  rtc_gpio_isolate(GPIO_NUM_15); // SD Command

  // 3. Clear serial buffer and trigger true deep sleep
  Serial.println("[INFO] Entering Deep Sleep. See you in 10 minutes!");
  Serial.flush();
  esp_deep_sleep_start();
  
  Serial.println("This will never be printed");
}