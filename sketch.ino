// ============================================================================
// BLYNK: define ini WAJIB di atas semua #include
// ============================================================================
#define BLYNK_TEMPLATE_ID   "TMPL6wNsf5Xrq"
#define BLYNK_TEMPLATE_NAME "Rainforest Guard"
#define BLYNK_AUTH_TOKEN    "[Credential]"
#define BLYNK_PRINT Serial

#include <Wire.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <BlynkSimpleEsp32.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ============================================================================
// KONFIGURASI JARINGAN
// ============================================================================
const char* ssid = "Wokwi-GUEST";
const char* pass = "";

const char* mqtt_broker = "broker.hivemq.com";
const int   mqtt_port   = 1883;
#define MQTT_PREFIX     "rainforestguard_cetta"
#define MQTT_CLIENT_ID  "rfg_cetta_gateway_01"

// ================= TELEGRAM =================
const char* BOT_TOKEN = "[credential]";
const int LONGPOLL_S = 2;           
#define MAX_SUB 20

int64_t subs[MAX_SUB];             
int subCount = 0;
int64_t tgOffset = 0;                
volatile bool alarmPending[2] = {false, false};  
TaskHandle_t tgTaskHandle;
Preferences prefs;               
WiFiClientSecure tgClient;
// ============================================

WiFiClient espClient;
PubSubClient mqtt_client(espClient);

// Interval (ms)
const unsigned long SIKLUS_MS         = 2000;
const unsigned long MQTT_INTERVAL_MS  = 3000;
const unsigned long BLYNK_INTERVAL_MS = 3000;
const unsigned long RETRY_WIFI_MS     = 10000;
const unsigned long RETRY_MQTT_MS     = 10000;
const unsigned long RETRY_BLYNK_MS    = 15000;

unsigned long lastSiklus = 0, lastMQTT = 0, lastBlynk = 0;
unsigned long lastWifiTry = 0;
unsigned long lastMqttTry = -RETRY_MQTT_MS;
unsigned long lastBlynkTry = -RETRY_BLYNK_MS;

// ============================================================================
// PIN DAN AMBANG
// ============================================================================
#define LED_HIJAU 25
#define LED_KUNING 26
#define LED_MERAH 27
#define BUZZER 19

const float KERING_PCT   = 70;
const int   GAS_WASPADA  = 2000;
const int   GAS_BAHAYA   = 3000;
const float SUHU_WASPADA = 40;
const float SUHU_BAHAYA  = 55;
const float TAMPER_G     = 0.3;
const int   BATAS_GAGAL  = 3;

const int SERVO_TUTUP = 0;
const int SERVO_A = 45, SERVO_B = 135;

enum Status { AMAN, WASPADA, BAHAYA, HILANG };
const char* namaStatus[] = {"AMAN", "WASPADA", "BAHAYA", "HILANG"};

// ============================================================================
// HARDWARE
// ============================================================================
DHT dht1(5, DHT22);
DHT dht2(14, DHT22);
Servo servo1, servo2;
Adafruit_SSD1306 oled(128, 64, &Wire, -1);

struct NodeCoordinates {
  float latitude;
  float longitude;
};

struct Node {
  const char* nama;
  int soilPin, gasPin;
  uint8_t mpuAddr;
  DHT* dht;
  int servoPin;
  Servo* servo;
  int gagal;
  Status status;
  float kering, suhu;
  int gas;
  bool sprinkler;
  int amanCount;
  bool tamper;
  NodeCoordinates coords;
};

Node nodes[2] = {
  {"Node 1", 35, 32, 0x69, &dht1, 18, &servo1, 0, AMAN, 0, 0, 0, false, 0, false, {-2.2104, 113.9160}},
  {"Node 2", 34, 33, 0x68, &dht2, 23, &servo2, 0, AMAN, 0, 0, 0, false, 0, false, {-2.2150, 113.9200}}
};

Status statusLama[2] = {AMAN, AMAN};  // untuk deteksi perubahan status (edge trigger)
int tick = 0;
Status statusSistem = AMAN;

// ============================================================================
// FUNGSI TELEGRAM
// ============================================================================
void simpanSubs() {
  prefs.begin("tg", false);
  prefs.putUChar("n", subCount);
  if (subCount) prefs.putBytes("ids", subs, sizeof(int64_t) * subCount);
  prefs.end();
}

void muatSubs() {
  prefs.begin("tg", false);
  subCount = prefs.getUChar("n", 0);
  if (subCount > MAX_SUB) subCount = 0;
  if (subCount) prefs.getBytes("ids", subs, sizeof(int64_t) * subCount);
  prefs.end();
  Serial.printf("[TG] %d pelanggan dimuat dari memori\n", subCount);
}

int cariSub(int64_t id) {
  for (int i = 0; i < subCount; i++) if (subs[i] == id) return i;
  return -1;
}

bool tambahSub(int64_t id) {
  if (cariSub(id) >= 0) return true;
  if (subCount >= MAX_SUB) return false;
  subs[subCount++] = id;
  simpanSubs();
  return true;
}

void hapusSub(int64_t id) {
  int i = cariSub(id);
  if (i < 0) return;
  subs[i] = subs[--subCount];
  simpanSubs();
}

int kirimPesan(int64_t chatId, const String &teks) {
  HTTPClient http;
  String url = String("https://api.telegram.org/bot") + BOT_TOKEN + "/sendMessage";
  if (!http.begin(tgClient, url)) return -1;
  http.setReuse(true);   
  http.setTimeout(6000);
  http.addHeader("Content-Type", "application/json");
  JsonDocument d;
  d["chat_id"] = chatId;
  d["text"] = teks;
  String body;
  serializeJson(d, body);
  int code = http.POST(body);
  http.end();
  return code;
}

void broadcast(const String &teks) {
  int i = 0;
  while (i < subCount) {
    int64_t id = subs[i];
    int code = kirimPesan(id, teks);
    Serial.printf("[TG] kirim ke #%d -> HTTP %d\n", i, code);
    if (code == 403) hapusSub(id);  
    else i++;
  }
}

String ringkasNode(Node &n) {
  String s = String(n.nama) + ": " + namaStatus[n.status] + "\n";
  if (n.status == HILANG) return s + "Sensor tidak merespon\n";
  s += "Suhu: " + String(n.suhu, 1) + " C\n";
  s += "Gas: " + String(n.gas) + "\n";
  s += "Kekeringan tanah: " + String(n.kering, 0) + "%\n";
  s += String("Sprinkler: ") + (n.sprinkler ? "ON" : "OFF") + "\n";
  if (n.tamper) s += "TAMPER terdeteksi!\n";
  return s;
}

void kirimAlarm(Node &n) {
  String t = "🚨 PERINGATAN BAHAYA KEBAKARAN 🚨\n\n" + ringkasNode(n);
  t += "\nSegera cek lokasi!";
  Serial.println("[TG] Broadcast alarm " + String(n.nama));
  broadcast(t);
}

void handlePesan(int64_t chatId, const String &text, const char* nama) {
  if (text.startsWith("/start")) {
    if (tambahSub(chatId)) {
      kirimPesan(chatId, String("Halo ") + nama + "! ✅ Kamu terdaftar.\n"
                 "Kamu akan menerima notifikasi jika ada node berstatus BAHAYA.\n\n"
                 "Perintah:\n/status - lihat kondisi terkini\n/stop - berhenti berlangganan");
    } else {
      kirimPesan(chatId, "Maaf, kuota pelanggan penuh.");
    }
  } else if (text.startsWith("/stop")) {
    hapusSub(chatId);
    kirimPesan(chatId, "Kamu berhenti berlangganan. Kirim /start untuk berlangganan lagi.");
  } else if (text.startsWith("/status")) {
    String t = "📊 Status saat ini\n\n";
    for (int i = 0; i < 2; i++) t += ringkasNode(nodes[i]) + "\n";
    kirimPesan(chatId, t);
  }
}

void pollTelegram() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  char off[24];
  snprintf(off, sizeof(off), "%lld", (long long)tgOffset);
  String url = String("https://api.telegram.org/bot") + BOT_TOKEN +
               "/getUpdates?limit=5&timeout=" + String(LONGPOLL_S) + "&allowed_updates=%5B%22message%22%5D&offset=" + off;
  if (!http.begin(tgClient, url)) return;
  http.setReuse(true);
  http.setTimeout((LONGPOLL_S + 5) * 1000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[TG] getUpdates gagal HTTP %d\n", code);
    http.end();
    return;
  }
  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return;
  for (JsonObject u : doc["result"].as<JsonArray>()) {
    tgOffset = (int64_t)u["update_id"] + 1;  
    JsonObject m = u["message"];
    if (m.isNull()) continue;
    int64_t chatId = m["chat"]["id"];
    String text = m["text"] | "";
    const char* nama = m["from"]["first_name"] | "teman";
    Serial.printf("[TG] pesan dari %s: %s\n", nama, text.c_str());
    handlePesan(chatId, text, nama);
  }
}

void telegramTask(void* pv) {
  for (;;) {
    for (int i = 0; i < 2; i++) {
      if (alarmPending[i]) {
        alarmPending[i] = false;
        kirimAlarm(nodes[i]);
      }
    }
    pollTelegram();           
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// ============================================================================
// KONEKSI (tidak memblokir sistem)
// ============================================================================
void jagaKoneksi(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiTry >= RETRY_WIFI_MS) {
      lastWifiTry = now;
      WiFi.disconnect();
      WiFi.begin(ssid, pass);
    }
    return;
  }
  if (!mqtt_client.connected() && now - lastMqttTry >= RETRY_MQTT_MS) {
    lastMqttTry = now;
    Serial.print("[MQTT] Menghubungkan...");
    if (mqtt_client.connect(MQTT_CLIENT_ID)) Serial.println("OK");
    else Serial.printf("gagal, kode %d\n", mqtt_client.state());
  }
  if (!Blynk.connected() && now - lastBlynkTry >= RETRY_BLYNK_MS) {
    lastBlynkTry = now;
    Serial.println("[BLYNK] Menghubungkan...");
    Blynk.connect(3000);
  }
}

// ============================================================================
// MQTT
// ============================================================================
void pub(int idx, const char* field, const char* val) {
  char topic[64];
  snprintf(topic, sizeof(topic), "%s/node%d/%s", MQTT_PREFIX, idx + 1, field);
  mqtt_client.publish(topic, val);
}

void kirimDataKeMQTT() {
  if (!mqtt_client.connected()) return;
  char b[16];

  for (int i = 0; i < 2; i++) {
    Node &n = nodes[i];
    snprintf(b, sizeof(b), "%.2f", n.suhu);                 pub(i, "temperature", b);
    snprintf(b, sizeof(b), "%d", n.gas);                    pub(i, "gas", b);
    snprintf(b, sizeof(b), "%.1f", n.kering);               pub(i, "soil_moisture", b);
    snprintf(b, sizeof(b), "%d", (int)n.status);            pub(i, "status", b);
    snprintf(b, sizeof(b), "%d", n.tamper ? 1 : 0);         pub(i, "tamper", b);
    snprintf(b, sizeof(b), "%d", n.sprinkler ? 1 : 0);      pub(i, "sprinkler", b);
    snprintf(b, sizeof(b), "%.4f", n.coords.latitude);      pub(i, "latitude", b);
    snprintf(b, sizeof(b), "%.4f", n.coords.longitude);     pub(i, "longitude", b);
  }

  char topic[64];
  snprintf(topic, sizeof(topic), "%s/system/status", MQTT_PREFIX);
  snprintf(b, sizeof(b), "%d", (int)statusSistem);
  mqtt_client.publish(topic, b);

  Serial.println("[MQTT] Data terkirim");
}

// ============================================================================
// BLYNK
// ============================================================================
void kirimDataKeBlynk() {
  if (!Blynk.connected()) return;

  Blynk.virtualWrite(V0, nodes[0].suhu);
  Blynk.virtualWrite(V1, nodes[0].gas);
  Blynk.virtualWrite(V2, nodes[0].kering);
  Blynk.virtualWrite(V3, (int)nodes[0].status);
  Blynk.virtualWrite(V4, nodes[0].tamper ? 1 : 0);
  Blynk.virtualWrite(V5, nodes[0].sprinkler ? 1 : 0);
  Blynk.virtualWrite(V6, nodes[0].coords.latitude);
  Blynk.virtualWrite(V7, nodes[0].coords.longitude);

  Blynk.virtualWrite(V8, nodes[1].suhu);
  Blynk.virtualWrite(V9, nodes[1].gas);
  Blynk.virtualWrite(V10, nodes[1].kering);
  Blynk.virtualWrite(V11, (int)nodes[1].status);
  Blynk.virtualWrite(V12, nodes[1].tamper ? 1 : 0);
  Blynk.virtualWrite(V13, nodes[1].sprinkler ? 1 : 0);
  Blynk.virtualWrite(V14, nodes[1].coords.latitude);
  Blynk.virtualWrite(V15, nodes[1].coords.longitude);

  Serial.println("[BLYNK] Data terkirim");
}

// ============================================================================
// MPU6050
// ============================================================================
bool bacaMPU(uint8_t addr, float &mag) {
  Wire.beginTransmission(addr);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, (uint8_t)6) != 6) return false;
  int16_t ax = (Wire.read() << 8) | Wire.read();
  int16_t ay = (Wire.read() << 8) | Wire.read();
  int16_t az = (Wire.read() << 8) | Wire.read();
  float x = ax / 16384.0, y = ay / 16384.0, z = az / 16384.0;
  mag = sqrt(x * x + y * y + z * z);
  return true;
}

void wakeMPU(uint8_t addr) {
  Wire.beginTransmission(addr);
  Wire.write(0x6B);
  Wire.write(0);
  Wire.endTransmission();
}

// ============================================================================
// EVALUASI STATUS NODE
// ============================================================================
Status evaluasi(Node &n) {
  float suhu = n.dht->readTemperature();
  float mag = 0;
  bool mpuOk = bacaMPU(n.mpuAddr, mag);

  int soilRaw = analogRead(n.soilPin);
  int gas = analogRead(n.gasPin);
  float kering = 100.0 - (soilRaw / 4095.0 * 100.0);

  n.kering = kering;
  n.gas = gas;
  n.suhu = isnan(suhu) ? 0 : suhu;
  n.tamper = false;

  if (isnan(suhu) || !mpuOk) n.gagal++;
  else n.gagal = 0;

  Serial.printf("[%s] kering=%.0f%% gas=%d suhu=%.1fC mpu=%s",
                n.nama, kering, gas, suhu, mpuOk ? "ok" : "MATI");

  if (n.gagal >= BATAS_GAGAL) {
    Serial.println(" -> HILANG");
    return HILANG;
  }

  bool tamper = mpuOk && fabs(mag - 1.0) > TAMPER_G;
  n.tamper = tamper;
  if (tamper) Serial.print(" TAMPER!");

  Status s = AMAN;
  if (gas > GAS_BAHAYA && suhu > SUHU_BAHAYA) s = BAHAYA;
  else if (gas > GAS_WASPADA || suhu > SUHU_WASPADA
           || kering > KERING_PCT || tamper) s = WASPADA;

  Serial.printf(" -> %s\n", namaStatus[s]);
  return s;
}

// ============================================================================
// SPRINKLER
// ============================================================================
void updateSprinkler(Node &n) {
  if (n.status == BAHAYA) {
    n.sprinkler = true;
    n.amanCount = 0;
  } else if (n.status != HILANG && n.sprinkler) {
    if (++n.amanCount >= 2) {
      n.sprinkler = false;
      n.amanCount = 0;
    }
  }
  if (n.sprinkler) n.servo->write(tick % 2 ? SERVO_B : SERVO_A);
  else n.servo->write(SERVO_TUTUP);
}

// ============================================================================
// OLED (8 baris x 21 karakter)
// ============================================================================
void tampilOLED(Status sistem) {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  for (int i = 0; i < 2; i++) {
    Node &n = nodes[i];
    int y = (i == 0) ? 0 : 29;

    oled.setCursor(0, y);
    oled.printf("N%d %-7s SPR:%s", i + 1, namaStatus[n.status],
                n.sprinkler ? "ON" : "OFF");

    if (n.status == HILANG) {
      oled.setCursor(0, y + 8);
      oled.print("SENSOR TDK RESPON");
    } else {
      oled.setCursor(0, y + 8);
      oled.printf("K%.0f%% G%d T%.1fC", n.kering, n.gas, n.suhu);
      oled.setCursor(0, y + 16);
      if (n.tamper)         oled.print("!! TAMPER TERDETEKSI");
      else if (n.sprinkler) oled.print(">> SPRINKLER AKTIF");
      else                  oled.printf("%.3f,%.3f", n.coords.latitude, n.coords.longitude);
    }
  }

  oled.drawLine(0, 26, 127, 26, SSD1306_WHITE);
  oled.drawLine(0, 54, 127, 54, SSD1306_WHITE);
  oled.setCursor(0, 56);
  oled.printf("SISTEM: %s", namaStatus[sistem]);
  oled.display();
}

// ============================================================================
// OUTPUT GLOBAL (LED + BUZZER)
// ============================================================================
void outputGlobal(Status s) {
  digitalWrite(LED_HIJAU, s == AMAN);
  digitalWrite(LED_KUNING, s == WASPADA);
  digitalWrite(LED_MERAH, s == BAHAYA);

  if (s == BAHAYA) {
    tone(BUZZER, 2000);
  } else if (s == HILANG) {
    digitalWrite(LED_KUNING, HIGH);
    digitalWrite(LED_MERAH, HIGH);
    tone(BUZZER, 1000, 200);
  } else {
    noTone(BUZZER);
  }
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  pinMode(LED_HIJAU, OUTPUT);
  pinMode(LED_KUNING, OUTPUT);
  pinMode(LED_MERAH, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  dht1.begin();
  dht2.begin();

  for (auto &n : nodes) {
    wakeMPU(n.mpuAddr);
    n.servo->setPeriodHertz(50);
    n.servo->attach(n.servoPin, 500, 2400);
    n.servo->write(SERVO_TUTUP);
  }

  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED tidak ketemu");
  }
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 10);
  oled.print("Rainforest Guard");
  oled.setCursor(0, 25);
  oled.print("Connecting WiFi...");
  oled.display();

  // Tunggu WiFi maksimal 10 detik
  WiFi.begin(ssid, pass);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  Serial.println(WiFi.status() == WL_CONNECTED ? "[WiFi] Terhubung" : "[WiFi] Belum terhubung, lanjut offline");

  mqtt_client.setServer(mqtt_broker, mqtt_port);
  Blynk.config(BLYNK_AUTH_TOKEN);

  // Setup Telegram
  tgClient.setInsecure();
  muatSubs();
  xTaskCreatePinnedToCore(telegramTask, "telegram", 12288, NULL, 1, &tgTaskHandle, 0);

  oled.clearDisplay();
  oled.setCursor(0, 10);
  oled.print("Rainforest Guard");
  oled.setCursor(0, 25);
  oled.print(WiFi.status() == WL_CONNECTED ? "WiFi OK" : "Mode offline");
  oled.display();
  delay(1500);

  Serial.println("[SETUP] Sistem siap");
}

// ============================================================================
// LOOP UTAMA
// ============================================================================
void loop() {
  unsigned long now = millis();

  jagaKoneksi(now);
  if (mqtt_client.connected()) mqtt_client.loop();
  if (Blynk.connected()) Blynk.run();

  // Siklus lokal: sensor, sprinkler, OLED, LED, buzzer (jalan walau tanpa jaringan)
  if (now - lastSiklus >= SIKLUS_MS) {
    lastSiklus = now;

    Status terburuk = AMAN;
    for (int i = 0; i < 2; i++) {
      Node &n = nodes[i];
      n.status = evaluasi(n);
      updateSprinkler(n);
      Status s = n.status;
      if (s == BAHAYA || (s == HILANG && terburuk != BAHAYA) ||
          (s == WASPADA && terburuk == AMAN)) terburuk = s;
    }
    statusSistem = terburuk;

    // Deteksi edge trigger untuk Telegram alarm
    for (int i = 0; i < 2; i++) {
      if (nodes[i].status == BAHAYA && statusLama[i] != BAHAYA) alarmPending[i] = true;
      statusLama[i] = nodes[i].status;
    }

    tampilOLED(terburuk);
    Serial.printf("== Status sistem: %s ==\n\n", namaStatus[terburuk]);
    outputGlobal(terburuk);
    tick++;
  }

  // Pengiriman data
  if (now - lastMQTT >= MQTT_INTERVAL_MS) {
    lastMQTT = now;
    kirimDataKeMQTT();
  }
  if (now - lastBlynk >= BLYNK_INTERVAL_MS) {
    lastBlynk = now;
    kirimDataKeBlynk();
  }

  delay(10);
}
