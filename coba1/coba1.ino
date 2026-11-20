#include <Arduino.h>
#include <WiFi.h>
#include "esp_http_server.h"
#include <p1_inferencing.h>
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

enum Hasil {
  HASIL_BENAR,
  HASIL_SALAH,
  HASIL_ERROR
};

// Matikan proteksi brownout seawal mungkin sebelum setup/konstruktor lain berjalan
__attribute__((constructor)) void disableBrownoutEarly() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
}

// ==========================================
// 1. PENGATURAN WIFI & WEBSERVER
// ==========================================

// Silakan sesuaikan nama WiFi & Password hotspot / router Anda:
const char* WIFI_SSID = "bondorek";
const char* WIFI_PASS = "calonwapres2040";

// Jika WiFi di atas gagal terhubung, ESP32 otomatis menyalakan Access Point (Hotspot) sendiri:
const char* AP_SSID = "ESP32S3-CAM-AI";
const char* AP_PASS = "12345678";

httpd_handle_t httpd = NULL;
static const char* STREAM_CT = "multipart/x-mixed-replace;boundary=frame";
static const char* STREAM_BOUNDARY = "\r\n--frame\r\n";
static SemaphoreHandle_t camera_mutex = NULL;

// Status deteksi real-time untuk ditampilkan di web
volatile bool last_detected = false;
char last_label[32] = "Menunggu...";
float last_accuracy = 0.0f;
int last_timing_dsp = 0;
int last_timing_infer = 0;

// ==========================================
// 2. KOMUNIKASI UART KE ESP32-C6 (MASTER IOT)
// ==========================================
#define LINK_RX 2
#define LINK_TX 1
HardwareSerial LinkSerial(1);

// ==========================================
// 3. PENGATURAN MODEL EDGE IMPULSE
// ==========================================

#define LABEL_BOTOL "beer"
#define THRESHOLD 0.60f

#define CAM_WIDTH  320
#define CAM_HEIGHT 240

// ==========================================
// 3. PIN ESP32-S3-CAM HW-679
// ==========================================

#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM   4
#define SIOC_GPIO_NUM   5

#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM     8
#define Y3_GPIO_NUM     9
#define Y2_GPIO_NUM    11

#define VSYNC_GPIO_NUM  6
#define HREF_GPIO_NUM   7
#define PCLK_GPIO_NUM  13

// ==========================================
// 4. VARIABEL GLOBAL
// ==========================================

static bool camera_ready = false;
static uint8_t *snapshot_buf = nullptr;

// ==========================================
// 5. INFORMASI MEMORI
// ==========================================

void printMemory(const char *judul) {
  Serial.println();
  Serial.println(judul);
  Serial.printf("Free Heap       : %u bytes\n", (unsigned int)ESP.getFreeHeap());
  Serial.printf("Free PSRAM      : %u bytes\n", (unsigned int)ESP.getFreePsram());
  Serial.printf("Largest Heap    : %u bytes\n", (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  Serial.printf("Largest PSRAM   : %u bytes\n", (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

// ==========================================
// 6. INISIALISASI KAMERA
// ==========================================

bool initCamera() {
  camera_config_t config = {};

  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
#else
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
#endif

  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d0 = Y2_GPIO_NUM;

  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;

  config.xclk_freq_hz = 10000000;
  config.ledc_timer = LEDC_TIMER_0;
  config.ledc_channel = LEDC_CHANNEL_0;

  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_QVGA;

  config.fb_count = 2; // 2 buffer di PSRAM agar web streaming dan AI lancar
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Kamera gagal diinisialisasi: 0x%x\n", err);
    return false;
  }

  camera_ready = true;
  Serial.println("Kamera berhasil diinisialisasi!");
  return true;
}

// ==========================================
// 7. ALOKASI BUFFER
// ==========================================

bool allocateMemory() {
  if (!psramFound()) {
    Serial.println("PSRAM tidak ditemukan!");
    return false;
  }

  size_t buffer_size = CAM_WIDTH * CAM_HEIGHT * 3;
  Serial.printf("Ukuran buffer snapshot: %u bytes\n", (unsigned int)buffer_size);
  Serial.printf("Input model: %d x %d\n", EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT);

  snapshot_buf = (uint8_t *)ps_malloc(buffer_size);
  if (snapshot_buf == nullptr) {
    Serial.println("Alokasi PSRAM gagal!");
    return false;
  }

  Serial.println("Buffer PSRAM berhasil dialokasikan!");
  return true;
}

// ==========================================
// 8. AMBIL DAN PROSES GAMBAR
// ==========================================

bool captureImage() {
  if (!camera_ready || snapshot_buf == nullptr) {
    Serial.println("Kamera atau buffer belum siap!");
    return false;
  }

  if (camera_mutex && xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    return false;
  }

  camera_fb_t *fb = esp_camera_fb_get();
  if (fb == nullptr) {
    if (camera_mutex) xSemaphoreGive(camera_mutex);
    Serial.println("Gagal mengambil frame!");
    return false;
  }

  if (fb->format != PIXFORMAT_RGB565 || fb->width != CAM_WIDTH || fb->height != CAM_HEIGHT || fb->len < (CAM_WIDTH * CAM_HEIGHT * 2)) {
    Serial.printf("Format/ukuran tidak sesuai atau frame terputus (%u bytes), abaikan...\n", (unsigned int)fb->len);
    esp_camera_fb_return(fb);
    if (camera_mutex) xSemaphoreGive(camera_mutex);
    return false;
  }

  // Konversi RGB565 menjadi RGB888
  bool converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_RGB565, snapshot_buf);
  esp_camera_fb_return(fb);
  if (camera_mutex) xSemaphoreGive(camera_mutex);

  if (!converted) {
    Serial.println("Konversi RGB888 gagal!");
    return false;
  }

  // Resize in-place ke ukuran input model AI (96x96)
  int resize_status = ei::image::processing::crop_and_interpolate_rgb888(
    snapshot_buf,
    CAM_WIDTH,
    CAM_HEIGHT,
    snapshot_buf,
    EI_CLASSIFIER_INPUT_WIDTH,
    EI_CLASSIFIER_INPUT_HEIGHT
  );

  if (resize_status != EIDSP_OK) {
    Serial.printf("Resize gagal: %d\n", resize_status);
    return false;
  }

  return true;
}

// ==========================================
// 9. CALLBACK DATA EDGE IMPULSE
// ==========================================

static int getImageData(size_t offset, size_t length, float *output) {
  if (snapshot_buf == nullptr || output == nullptr) {
    return -1;
  }

  const size_t total_pixels = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
  if (offset > total_pixels || length > total_pixels - offset) {
    return -1;
  }

  size_t pixel_index = offset * 3;

  for (size_t i = 0; i < length; i++) {
    // Susunan kanal diperbaiki: Red, Green, Blue (bukan dibalik BGR)
    uint32_t pixel =
      ((uint32_t)snapshot_buf[pixel_index] << 16) |
      ((uint32_t)snapshot_buf[pixel_index + 1] << 8) |
      ((uint32_t)snapshot_buf[pixel_index + 2]);

    output[i] = (float)pixel;
    pixel_index += 3;
  }

  return 0;
}

// ==========================================
// 10. PROSES DETEKSI EDGE IMPULSE
// ==========================================

Hasil detectObject() {
#if EI_CLASSIFIER_OBJECT_DETECTION != 1
  Serial.println("Model bukan Object Detection!");
  return HASIL_ERROR;
#endif

  if (!captureImage()) {
    return HASIL_ERROR;
  }

  ei::signal_t signal;
  signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
  signal.get_data = getImageData;

  ei_impulse_result_t result = {0};
  EI_IMPULSE_ERROR status = run_classifier(&signal, &result, false);

  if (status != EI_IMPULSE_OK) {
    Serial.printf("Inferensi gagal: %d\n", status);
    return HASIL_ERROR;
  }

  last_timing_dsp = result.timing.dsp;
  last_timing_infer = result.timing.classification;

  bool bottle_found = false;
  float highest_score = 0.0f;
  char detected_obj[32] = "none";

  for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
    ei_impulse_result_bounding_box_t box = result.bounding_boxes[i];
    if (box.value < THRESHOLD) continue;

    Serial.printf("Objek: %s | Akurasi: %.2f\n", box.label, box.value);

    if (box.value > highest_score) {
      highest_score = box.value;
      strncpy(detected_obj, box.label, sizeof(detected_obj) - 1);
    }

    if (strcmp(box.label, LABEL_BOTOL) == 0) {
      bottle_found = true;
    }
  }

  // Update status untuk tampilan Web Dashboard
  last_detected = bottle_found;
  last_accuracy = highest_score;
  if (bottle_found) {
    strncpy(last_label, LABEL_BOTOL, sizeof(last_label) - 1);
  } else if (highest_score > 0.0f) {
    strncpy(last_label, detected_obj, sizeof(last_label) - 1);
  } else {
    strncpy(last_label, "TIDAK TERDETEKSI", sizeof(last_label) - 1);
  }

  return bottle_found ? HASIL_BENAR : HASIL_SALAH;
}

// ==========================================
// 11. HANDLER HTTP WEBSERVER
// ==========================================

static esp_err_t index_handler(httpd_req_t* req) {
  const char* html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-S3 Camera & AI Monitor</title>"
    "<style>"
    "body{margin:0;font-family:Arial,sans-serif;background:#181818;color:#eee;text-align:center;padding:12px;}"
    "h2{color:#4CAF50;margin-bottom:8px;}"
    ".stream-box{display:inline-block;max-width:640px;width:100%;border-radius:10px;overflow:hidden;box-shadow:0 4px 12px rgba(0,0,0,0.5);background:#000;}"
    "img{width:100%;display:block;}"
    ".badge{display:inline-block;padding:10px 20px;border-radius:24px;font-size:18px;font-weight:bold;margin:12px 0;transition:0.3s;}"
    ".badge-yes{background:#28a745;color:#fff;}"
    ".badge-no{background:#444;color:#bbb;}"
    ".info{color:#aaa;font-size:14px;margin-top:6px;}"
    ".btn{display:inline-block;padding:8px 16px;background:#007bff;color:#fff;text-decoration:none;border-radius:6px;margin-top:10px;font-size:14px;}"
    "</style></head><body>"
    "<h2>ESP32-S3 Smart Bin & AI Camera</h2>"
    "<div class='stream-box'><img src='/stream'></div><br>"
    "<div id='badge' class='badge badge-no'>Memuat status AI...</div>"
    "<div id='info' class='info'>Waktu inferensi: -</div><br>"
    "<a class='btn' href='/capture' target='_blank'>Ambil Foto JPEG</a>"
    "<script>"
    "setInterval(async()=>{"
    "  try{"
    "    let r=await fetch('/status');"
    "    let d=await r.json();"
    "    let b=document.getElementById('badge');"
    "    if(d.detected){"
    "      b.className='badge badge-yes';"
    "      b.innerHTML='&#x2705; ' + d.label.toUpperCase() + ' TERDETEKSI (' + (d.confidence*100).toFixed(0) + '%)';"
    "    }else{"
    "      b.className='badge badge-no';"
    "      b.innerHTML='&#x26AA; ' + d.label + (d.confidence>0?' (' + (d.confidence*100).toFixed(0) + '%)':'');"
    "    }"
    "    document.getElementById('info').innerText='DSP: '+d.dsp_ms+'ms | AI: '+d.infer_ms+'ms';"
    "  }catch(e){}"
    "}, 800);"
    "</script></body></html>";

  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t* req) {
  char json[160];
  snprintf(json, sizeof(json),
    "{\"detected\":%s,\"label\":\"%s\",\"confidence\":%.2f,\"dsp_ms\":%d,\"infer_ms\":%d}",
    last_detected ? "true" : "false",
    last_label,
    last_accuracy,
    last_timing_dsp,
    last_timing_infer
  );

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t capture_handler(httpd_req_t* req) {
  if (camera_mutex && xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    if (camera_mutex) xSemaphoreGive(camera_mutex);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  uint8_t* jpg = NULL;
  size_t jlen = 0;
  bool ok = frame2jpg(fb, 80, &jpg, &jlen);
  esp_camera_fb_return(fb);
  if (camera_mutex) xSemaphoreGive(camera_mutex);

  if (!ok) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  esp_err_t r = httpd_resp_send(req, (const char*)jpg, jlen);
  free(jpg);
  return r;
}

static esp_err_t stream_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, STREAM_CT);
  char part[64];

  while (true) {
    if (camera_mutex && xSemaphoreTake(camera_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb || fb->len < (CAM_WIDTH * CAM_HEIGHT * 2)) {
      if (fb) esp_camera_fb_return(fb);
      if (camera_mutex) xSemaphoreGive(camera_mutex);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    uint8_t* jpg = NULL;
    size_t jlen = 0;
    bool ok = frame2jpg(fb, 80, &jpg, &jlen);
    esp_camera_fb_return(fb);
    if (camera_mutex) xSemaphoreGive(camera_mutex);

    if (!ok) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    esp_err_t r = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (r == ESP_OK) {
      size_t n = snprintf(part, sizeof(part),
        "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", jlen);
      r = httpd_resp_send_chunk(req, part, n);
    }
    if (r == ESP_OK) {
      r = httpd_resp_send_chunk(req, (const char*)jpg, jlen);
    }
    free(jpg);

    if (r != ESP_OK) break;
    vTaskDelay(pdMS_TO_TICKS(30));
  }
  return ESP_OK;
}

void startServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_uri_handlers = 8;
  cfg.stack_size = 8192;

  if (httpd_start(&httpd, &cfg) == ESP_OK) {
    httpd_uri_t idx = {"/", HTTP_GET, index_handler, NULL};
    httpd_uri_t str = {"/stream", HTTP_GET, stream_handler, NULL};
    httpd_uri_t cap = {"/capture", HTTP_GET, capture_handler, NULL};
    httpd_uri_t sta = {"/status", HTTP_GET, status_handler, NULL};

    httpd_register_uri_handler(httpd, &idx);
    httpd_register_uri_handler(httpd, &str);
    httpd_register_uri_handler(httpd, &cap);
    httpd_register_uri_handler(httpd, &sta);
    Serial.println("Web Server kamera & AI berhasil dimulai!");
  }
}

// ==========================================
// 12. SETUP WIFI
// ==========================================

void setupWiFi() {
  Serial.print("Menghubungkan ke WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_17dBm); // Sedikit turunkan daya pancar puncak untuk stabilitas arus listrik modul
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(">>> Terhubung ke WiFi! Buka Web Server di: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Gagal terhubung ke WiFi router/hotspot.");
    Serial.println("Menyalakan Access Point (Hotspot) mandiri...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("Hubungkan HP/Laptop ke WiFi: ");
    Serial.println(AP_SSID);
    Serial.print(">>> Lalu buka browser di: http://");
    Serial.println(WiFi.softAPIP());
  }
}

// ==========================================
// 13. SETUP
// ==========================================

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Nonaktifkan brownout detector agar tidak restart saat lonjakan arus Wi-Fi

  Serial.begin(115200);
  delay(1500);

  // Inisialisasi UART komunikasi ke ESP32-C6
  LinkSerial.begin(115200, SERIAL_8N1, LINK_RX, LINK_TX);
  LinkSerial.setTimeout(300);

  Serial.println();
  Serial.println("==========================================");
  Serial.println("ESP32-S3 SMART TRASH BIN + WEBSERVER CAM");
  Serial.println("EDGE IMPULSE FOMO OBJECT DETECTION");
  Serial.println("==========================================");

  camera_mutex = xSemaphoreCreateMutex();

  printMemory("Memori awal");

  Serial.println("\nMemulai alokasi buffer...");
  if (!allocateMemory()) {
    Serial.println("Gagal alokasi buffer!");
    while (true) delay(1000);
  }

  Serial.println("\nMemulai kamera...");
  if (!initCamera()) {
    Serial.println("Kamera gagal diinisialisasi!");
    while (true) delay(1000);
  }

  delay(200); // Beri jeda agar tegangan listrik stabil sebelum menyalakan radio Wi-Fi

  Serial.println("\nMemulai koneksi WiFi...");
  setupWiFi();

  Serial.println("\nMemulai HTTP Web Server...");
  startServer();

  // Beritahu ESP32-C6 bahwa kamera & AI sudah siap
  LinkSerial.println("READY");
  Serial.println("[UART] Mengirim status READY ke ESP32-C6");

  Serial.println("\n>>> SISTEM, KAMERA & UART SIAP! <<<");
}

// ==========================================
// 14. LOOP UTAMA
// ==========================================

void loop() {
  // 1. Cek perintah masuk dari ESP32-C6 (UART)
  if (LinkSerial.available()) {
    String cmd = LinkSerial.readStringUntil('\n');
    cmd.trim();

    if (cmd == "CLASSIFY") {
      LinkSerial.println("BUSY");
      Serial.println("\n[UART ESP32-C6] Menerima instruksi CLASSIFY!");

      Hasil hasil = detectObject();

      if (hasil == HASIL_BENAR) {
        LinkSerial.println("BOTOL_PLASTIK");
        Serial.println("[UART ESP32-C6] Balasan dikirim: BOTOL_PLASTIK");
      } else if (hasil == HASIL_SALAH) {
        LinkSerial.println("BUKAN_BOTOL");
        Serial.println("[UART ESP32-C6] Balasan dikirim: BUKAN_BOTOL");
      } else {
        LinkSerial.println("ERROR");
        Serial.println("[UART ESP32-C6] Balasan dikirim: ERROR");
      }
      return;
    }
  }

  // 2. Mode Mandiri (Standalone): Update deteksi berkala setiap 1.5 detik
  // agar status di Web Server tetap live meskipun tidak ada ESP32-C6 yang terhubung
  static unsigned long last_detect_time = 0;
  if (millis() - last_detect_time > 1500) {
    last_detect_time = millis();
    detectObject();
  }

  delay(20);
}
