#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"

const char* WIFI_SSID = "Android AP";
const char* WIFI_PASS = "12345678";

#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5
#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM    8
#define Y3_GPIO_NUM    9
#define Y2_GPIO_NUM    11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

httpd_handle_t httpd = NULL;

static const char* STREAM_CT = "multipart/x-mixed-replace;boundary=frame";
static const char* STREAM_BOUNDARY = "\r\n--frame\r\n";

static esp_err_t index_handler(httpd_req_t* req) {
  const char* html =
    "<html><body style='margin:0;background:#000;text-align:center'>"
    "<h3 style='color:#fff'>ESP32-S3-CAM</h3>"
    "<img src='/stream' style='width:100%;max-width:640px'><br>"
    "<a style='color:#0af' href='/capture'>Ambil foto</a>"
    "</body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t capture_handler(httpd_req_t* req) {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { httpd_resp_send_500(req); return ESP_FAIL; }
  uint8_t* jpg = NULL;
  size_t jlen = 0;
  bool ok = frame2jpg(fb, 80, &jpg, &jlen);
  esp_camera_fb_return(fb);
  if (!ok) { httpd_resp_send_500(req); return ESP_FAIL; }
  httpd_resp_set_type(req, "image/jpeg");
  esp_err_t r = httpd_resp_send(req, (const char*)jpg, jlen);
  free(jpg);
  return r;
}

static esp_err_t stream_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, STREAM_CT);
  char part[64];
  while (true) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return ESP_FAIL;
    uint8_t* jpg = NULL;
    size_t jlen = 0;
    bool ok = frame2jpg(fb, 80, &jpg, &jlen);
    esp_camera_fb_return(fb);
    if (!ok) return ESP_FAIL;

    esp_err_t r = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (r == ESP_OK) {
      size_t n = snprintf(part, sizeof(part),
        "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", jlen);
      r = httpd_resp_send_chunk(req, part, n);
    }
    if (r == ESP_OK) r = httpd_resp_send_chunk(req, (const char*)jpg, jlen);
    free(jpg);
    if (r != ESP_OK) break;
  }
  return ESP_OK;
}

void startServer() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_uri_handlers = 8;
  if (httpd_start(&httpd, &cfg) == ESP_OK) {
    httpd_uri_t idx = {"/", HTTP_GET, index_handler, NULL};
    httpd_uri_t cap = {"/capture", HTTP_GET, capture_handler, NULL};
    httpd_uri_t str = {"/stream", HTTP_GET, stream_handler, NULL};
    httpd_register_uri_handler(httpd, &idx);
    httpd_register_uri_handler(httpd, &cap);
    httpd_register_uri_handler(httpd, &str);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("PSRAM: %s, ukuran: %u byte\n", psramFound() ? "ada" : "TIDAK ada", ESP.getPsramSize());

  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_RGB565;
  c.frame_size = FRAMESIZE_QVGA;
  c.jpeg_quality = 12;
  c.fb_count = 1;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return;
  }

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(false);
  Serial.print("Menghubungkan WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println();

  startServer();
  Serial.print("Buka: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  delay(10000);
}