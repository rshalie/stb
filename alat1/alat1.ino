
#include <Arduino.h>
#include <Wire.h>
#include <Keypad.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>

// =============================================
// 1. KONFIGURASI PIN ESP32-C6
// =============================================

// LCD I2C
#define LCD_SDA 3
#define LCD_SCL 2

// Sensor Infrared
#define IR_PIN 5

// Servo
#define SERVO1_PIN 7
#define SERVO2_PIN 6

// UART ESP32-S3-CAM
#define CAM_RX 0
#define CAM_TX 1

// =============================================
// 2. KONFIGURASI WIFI DAN SERVER
// =============================================

const char* WIFI_SSID = "NAMA_WIFI";
const char* WIFI_PASSWORD = "PASSWORD_WIFI";

// Ganti IP dengan IP laptop XAMPP kamu
const char* SERVER_URL =
  "http://192.168.1.10/smarttrash/tambah_reward.php";

// =============================================
// 3. KONFIGURASI REWARD
// =============================================

const int REWARD_PER_BOTTLE = 1000;

int totalBottle = 0;
long totalReward = 0;

// =============================================
// 4. OBJEK KOMPONEN
// =============================================

LiquidCrystal_I2C lcd(0x27, 16, 2);

Servo servo1;
Servo servo2;

HardwareSerial CamSerial(1);

// =============================================
// 5. KEYPAD 4X4
// =============================================

const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

byte rowPins[ROWS] = {
  12, 13, 9, 18
};

byte colPins[COLS] = {
  19, 20, 21, 22
};

Keypad keypad = Keypad(
  makeKeymap(keys),
  rowPins,
  colPins,
  ROWS,
  COLS
);

// =============================================
// 6. VARIABEL SISTEM
// =============================================

const int IR_ACTIVE = LOW;

bool objectDetected = false;
bool waitingAI = false;

bool danaTerdaftar = false;

String nomorDana = "";
String inputDana = "";

unsigned long aiStartTime = 0;

const unsigned long AI_TIMEOUT = 15000;

// =============================================
// 7. PROTOTYPE FUNGSI
// =============================================

void connectWiFi();
void readKeypad();
void readInfrared();
void readCamera();
void acceptBottle();
void checkTimeout();
void showHome();
void sendDanaDisplay();
bool sendRewardToDatabase();

// =============================================
// 8. SETUP
// =============================================

void setup() {

  Serial.begin(115200);

  // UART KAMERA
  CamSerial.begin(
    115200,
    SERIAL_8N1,
    CAM_RX,
    CAM_TX
  );

  CamSerial.setTimeout(300);

  // LCD
  Wire.begin(LCD_SDA, LCD_SCL);

  lcd.init();
  lcd.backlight();

  // WIFI
  connectWiFi();

  // INFRARED
  pinMode(IR_PIN, INPUT);

  // SERVO
  servo1.setPeriodHertz(50);
  servo2.setPeriodHertz(50);

  servo1.attach(SERVO1_PIN, 500, 2400);
  servo2.attach(SERVO2_PIN, 500, 2400);

  servo1.write(0);
  servo2.write(0);

  // TAMPILAN AWAL
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SMART TRASH BIN");

  lcd.setCursor(0, 1);
  lcd.print("System Ready");

  Serial.println();
  Serial.println("SMART TRASH BIN STARTED");

  delay(1500);

  showHome();
}

// =============================================
// 9. LOOP UTAMA
// =============================================

void loop() {

  readKeypad();

  readInfrared();

  readCamera();

  checkTimeout();

}

// =============================================
// 10. KONEKSI WIFI
// =============================================

void connectWiFi() {

  WiFi.mode(WIFI_STA);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  lcd.clear();
  lcd.print("Menghubungkan");
  lcd.setCursor(0, 1);
  lcd.print("WiFi...");

  Serial.print("Menghubungkan WiFi");

  unsigned long start = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < 20000
  ) {

    delay(500);

    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println();
    Serial.println("WiFi Terhubung!");

    Serial.print("IP ESP32: ");
    Serial.println(WiFi.localIP());

    lcd.clear();
    lcd.print("WiFi Terhubung");

  } else {

    Serial.println();
    Serial.println("WiFi Gagal!");

    lcd.clear();
    lcd.print("WiFi Gagal");

  }

  delay(1500);
}

// =============================================
// 11. KEYPAD - INPUT NOMOR DANA
// =============================================

void readKeypad() {

  char key = keypad.getKey();

  if (!key) return;

  Serial.print("Key: ");
  Serial.println(key);

  // GANTI NOMOR DANA
  if (key == 'D') {

    nomorDana = "";
    inputDana = "";

    danaTerdaftar = false;

    sendDanaDisplay();

    return;
  }

  // Jika nomor sudah tersimpan,
  // abaikan tombol lainnya.
  if (danaTerdaftar) {
    return;
  }

  // INPUT ANGKA
  if (key >= '0' && key <= '9') {

    if (inputDana.length() < 13) {

      inputDana += key;
    }
  }

  // HAPUS ANGKA TERAKHIR
  else if (key == '*') {

    if (inputDana.length() > 0) {

      inputDana.remove(
        inputDana.length() - 1
      );
    }
  }

  // SIMPAN NOMOR DANA
  else if (key == '#') {

    if (
      inputDana.length() >= 10 &&
      inputDana.length() <= 13
    ) {

      nomorDana = inputDana;

      danaTerdaftar = true;

      inputDana = "";

      lcd.clear();
      lcd.print("Nomor Disimpan");

      Serial.print("Nomor DANA: ");
      Serial.println(nomorDana);

      delay(1000);

    } else {

      lcd.clear();
      lcd.print("Nomor Tidak Valid");

      delay(1000);
    }
  }

  sendDanaDisplay();
}

// =============================================
// 12. TAMPILAN INPUT DANA
// =============================================

void sendDanaDisplay() {

  lcd.clear();

  if (danaTerdaftar) {

    lcd.setCursor(0, 0);
    lcd.print("DANA Terdaftar");

    lcd.setCursor(0, 1);
    lcd.print(nomorDana);

  } else {

    lcd.setCursor(0, 0);
    lcd.print("Input No DANA:");

    lcd.setCursor(0, 1);
    lcd.print(inputDana);
  }
}

// =============================================
// 13. SENSOR INFRARED
// =============================================

void readInfrared() {

  bool detected =
    digitalRead(IR_PIN) == IR_ACTIVE;

  // Nomor DANA wajib dimasukkan
  if (!danaTerdaftar) {
    return;
  }

  if (
    detected &&
    !objectDetected &&
    !waitingAI
  ) {

    objectDetected = true;
    waitingAI = true;

    aiStartTime = millis();

    lcd.clear();
    lcd.print("Objek Terdeteksi");

    lcd.setCursor(0, 1);
    lcd.print("Memproses AI...");

    Serial.println("Mengirim CLASSIFY");

    CamSerial.println("CLASSIFY");
  }

  if (!detected && objectDetected) {

    objectDetected = false;
  }
}

// =============================================
// 14. MENERIMA HASIL AI DARI KAMERA
// =============================================

void readCamera() {

  if (!CamSerial.available()) {
    return;
  }

  String result =
    CamSerial.readStringUntil('\n');

  result.trim();

  if (result.length() == 0) {
    return;
  }

  Serial.print("Kamera: ");
  Serial.println(result);

  if (result == "READY") {

    lcd.clear();
    lcd.print("Kamera Siap");

    delay(1000);

    showHome();
  }

  else if (result == "BUSY") {

    lcd.clear();
    lcd.print("AI Memproses...");
  }

  else if (result == "BOTOL_PLASTIK") {

    waitingAI = false;

    acceptBottle();
  }

  else if (result == "BUKAN_BOTOL") {

    waitingAI = false;

    lcd.clear();
    lcd.print("Bukan Botol!");

    lcd.setCursor(0, 1);
    lcd.print("Tidak Diterima");

    servo1.write(0);
    servo2.write(0);

    delay(1500);

    showHome();
  }

  else if (result == "ERROR") {

    waitingAI = false;

    lcd.clear();
    lcd.print("AI ERROR");

    servo1.write(0);
    servo2.write(0);

    delay(1500);

    showHome();
  }
}

// =============================================
// 15. BOTOL PLASTIK DITERIMA
// =============================================

void acceptBottle() {

  Serial.println("BOTOL PLASTIK DITERIMA");

  lcd.clear();
  lcd.print("Botol Diterima");

  // BUKA PINTU INPUT
  servo1.write(90);

  delay(800);

  // MEKANISME SORTIR
  servo2.write(90);

  delay(1000);

  // KEMBALIKAN SERVO 2
  servo2.write(0);

  delay(500);

  // TUTUP PINTU INPUT
  servo1.write(0);

  delay(500);

  // KIRIM REWARD KE DATABASE
  lcd.clear();
  lcd.print("Mengirim Reward");

  bool saved = sendRewardToDatabase();

  if (saved) {

    totalBottle++;

    totalReward += REWARD_PER_BOTTLE;

    Serial.println("REWARD BERHASIL DISIMPAN");

    Serial.print("Total Botol: ");
    Serial.println(totalBottle);

    Serial.print("Total Reward: Rp");
    Serial.println(totalReward);

    lcd.clear();
    lcd.print("Reward Tersimpan");

    lcd.setCursor(0, 1);
    lcd.print("+Rp1000");

  } else {

    Serial.println("GAGAL SIMPAN REWARD");

    lcd.clear();
    lcd.print("Gagal Simpan!");

    lcd.setCursor(0, 1);
    lcd.print("Cek WiFi/Server");
  }

  delay(2000);

  showHome();
}

// =============================================
// 16. MENGIRIM DATA KE MYSQL MELALUI PHP
// =============================================

bool sendRewardToDatabase() {

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("WiFi Tidak Terhubung");

    return false;
  }

  if (!danaTerdaftar) {

    Serial.println("Nomor DANA Kosong");

    return false;
  }

  WiFiClient client;

  HTTPClient http;

  http.setTimeout(8000);

  if (!http.begin(client, SERVER_URL)) {

    Serial.println("Gagal Membuka Server");

    return false;
  }

  http.addHeader(
    "Content-Type",
    "application/json"
  );

  // MEMBUAT DATA JSON
  String jsonData = "{";

  jsonData += "\"nomor_dana\":\"";
  jsonData += nomorDana;
  jsonData += "\",";

  jsonData += "\"jumlah_botol\":1,";

  jsonData += "\"reward\":";
  jsonData += String(REWARD_PER_BOTTLE);

  jsonData += "}";

  Serial.println("======================");
  Serial.println("KIRIM DATA DATABASE");
  Serial.println(jsonData);

  // HTTP POST
  int httpCode = http.POST(jsonData);

  String response = "";

  if (httpCode > 0) {

    response = http.getString();

    Serial.print("HTTP Code: ");
    Serial.println(httpCode);

    Serial.print("Response: ");
    Serial.println(response);

  } else {

    Serial.print("HTTP Error: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();

  // PERIKSA RESPONS SERVER
  bool success =
    httpCode >= 200 &&
    httpCode < 300 &&
    response.indexOf("success") >= 0;

  return success;
}

// =============================================
// 17. TIMEOUT AI
// =============================================

void checkTimeout() {

  if (
    waitingAI &&
    millis() - aiStartTime > AI_TIMEOUT
  ) {

    waitingAI = false;

    lcd.clear();
    lcd.print("Kamera Timeout");

    Serial.println("AI TIMEOUT");

    servo1.write(0);
    servo2.write(0);

    delay(1500);

    showHome();
  }
}

// =============================================
// 18. TAMPILAN UTAMA
// =============================================

void showHome() {

  lcd.clear();

  if (!danaTerdaftar) {

    lcd.setCursor(0, 0);
    lcd.print("Input No DANA");

    lcd.setCursor(0, 1);
    lcd.print("Tekan Angka");

  } else {

    lcd.setCursor(0, 0);
    lcd.print("SMART TRASH BIN");

    lcd.setCursor(0, 1);
    lcd.print("Botol:");

    lcd.print(totalBottle);
  }
}
