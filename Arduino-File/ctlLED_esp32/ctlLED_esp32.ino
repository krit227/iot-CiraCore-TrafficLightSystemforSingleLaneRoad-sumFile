#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>

// -------------------- GPIO / CONFIG --------------------
// เสา A
#define A_RED_PIN    19
#define A_GREEN_PIN  18
// เสา B
#define B_RED_PIN    25
#define B_GREEN_PIN  26

// ===== ใส่ Wi-Fi ที่นี่ =====
const char* WIFI_SSID     = "IotTestK2.4G";
const char* WIFI_PASSWORD = "44445555";

// mDNS hostname ของ MQTT Broker (เครื่องโน้ตบุ๊ก Windows 10)
const char* BROKER_MDNS_HOST = "broker-mqtt";   // ใช้เป็น broker-mqtt.local
const uint16_t BROKER_PORT   = 1883;

WiFiClient    espClient;
PubSubClient  client(espClient);
IPAddress     brokerIP;
unsigned long lastReconnectAttempt = 0;

// -------------------- สถานะภายใน --------------------
String currentMode = "manual";   // "manual" | "auto"
String stateA = "RED";           // "RED" | "GREEN"
String stateB = "RED";           // "RED" | "GREEN"

// -------------------- Helper: จัดการไฟตามเสา/สถานะ --------------------
void setPoleRed(char pole) {
  if (pole == 'A') {
    digitalWrite(A_GREEN_PIN, LOW);
    digitalWrite(A_RED_PIN, HIGH);
    stateA = "RED";
  } else if (pole == 'B') {
    digitalWrite(B_GREEN_PIN, LOW);
    digitalWrite(B_RED_PIN, HIGH);
    stateB = "RED";
  }
}

void setPoleGreen(char pole) {
  if (pole == 'A') {
    digitalWrite(A_RED_PIN, LOW);
    digitalWrite(A_GREEN_PIN, HIGH);
    stateA = "GREEN";
  } else if (pole == 'B') {
    digitalWrite(B_RED_PIN, LOW);
    digitalWrite(B_GREEN_PIN, HIGH);
    stateB = "GREEN";
  }
}

void setAllRed() {
  digitalWrite(A_GREEN_PIN, LOW);
  digitalWrite(B_GREEN_PIN, LOW);
  digitalWrite(A_RED_PIN, HIGH);
  digitalWrite(B_RED_PIN, HIGH);
  stateA = "RED";
  stateB = "RED";
}

// -------------------- Wi-Fi --------------------
void setup_wifi() {
  Serial.print("Connecting to ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++tries > 120) { // ~60s timeout
      Serial.println("\n[WiFi] connect timeout → restart");
      delay(1500);
      ESP.restart();
    }
  }

  Serial.println("\nWiFi connected.");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

// -------------------- mDNS → MQTT Broker resolve --------------------
bool resolveBrokerViaMDNS() {
  static bool mdnsStarted = false;
  if (!mdnsStarted) {
    if (!MDNS.begin("esp32-traffic")) {
      Serial.println("[mDNS] เริ่ม MDNS ไม่สำเร็จ");
      return false;
    }
    mdnsStarted = true;
  }

  Serial.printf("[mDNS] Query %s.local ...\n", BROKER_MDNS_HOST);
  IPAddress ip = MDNS.queryHost(BROKER_MDNS_HOST);
  if (ip == INADDR_NONE) {
    Serial.println("[mDNS] หา broker ไม่เจอ (queryHost = NONE)");
    return false;
  }

  brokerIP = ip;
  Serial.print("[mDNS] broker IP = ");
  Serial.println(brokerIP);
  client.setServer(brokerIP, BROKER_PORT);
  return true;
}

// -------------------- Pins --------------------
void setup_pins() {
  pinMode(A_RED_PIN, OUTPUT);
  pinMode(A_GREEN_PIN, OUTPUT);
  pinMode(B_RED_PIN, OUTPUT);
  pinMode(B_GREEN_PIN, OUTPUT);
  setAllRed();  // ค่าเริ่มต้นเพื่อความปลอดภัย
}

// -------------------- MQTT Callback --------------------
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  String messageTemp;
  for (unsigned int i = 0; i < length; i++) messageTemp += (char)payload[i];

  Serial.print("[RECEIVED] "); Serial.print(topic); Serial.print(" → "); Serial.println(messageTemp);

  if (String(topic) == "traffic/control") {
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, messageTemp);
    if (error) {
      Serial.println("[ERROR] ไม่สามารถแปลง JSON ได้");
      return;
    }

    String side = doc["side"] | "";
    String state = doc["state"] | "";
    String mode = doc["mode"] | "";

    if (!(mode == "manual" || mode == "auto")) {
      Serial.println("[IGNORE] unknown mode");
      return;
    }
    currentMode = mode;

    if (side == "A") {
      if (state == "GREEN") setPoleGreen('A');
      else if (state == "RED") setPoleRed('A');
      else { Serial.print("[IGNORE] unknown state for A: "); Serial.println(state); return; }
    }
    else if (side == "B") {
      if (state == "GREEN") setPoleGreen('B');
      else if (state == "RED") setPoleRed('B');
      else { Serial.print("[IGNORE] unknown state for B: "); Serial.println(state); return; }
    }
    else if (side == "ALL") {
      if (state == "RED") setAllRed();
      else { Serial.print("[IGNORE] unsupported combo side=ALL state="); Serial.println(state); return; }
    }
    else {
      Serial.print("[IGNORE] unknown side: "); Serial.println(side);
      return;
    }

    Serial.print("[UPDATE] side=");
    Serial.print(side);
    Serial.print(" → state=");
    Serial.print(state);
    Serial.print(" (");
    Serial.print(mode);
    Serial.println(")");
  }
}

// -------------------- MQTT Reconnect --------------------
void mqtt_reconnect() {
  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 2000) {
      lastReconnectAttempt = now;

      // resolve mDNS หากยังไม่มี IP
      if (brokerIP == INADDR_NONE || brokerIP[0] == 0) {
        if (!resolveBrokerViaMDNS()) {
          Serial.println("[MQTT] ยัง resolve broker-mqtt.local ไม่ได้ จะลองใหม่...");
          return;
        }
      }

      Serial.print("เชื่อมต่อ MQTT...");
      String clientId = "ESP32Traffic-AB";
      if (client.connect(clientId.c_str())) {
        Serial.println("สำเร็จ");
        client.subscribe("traffic/control");
      } else {
        Serial.print("ล้มเหลว rc=");
        Serial.print(client.state());
        Serial.println(" จะลองใหม่ใน 2 วิ");
      }
    }
  }
}

// -------------------- Setup / Loop --------------------
void setup() {
  Serial.begin(115200);
  setup_pins();
  setup_wifi();

  client.setCallback(mqtt_callback);

  // resolve broker ครั้งแรก (ถ้าไม่เจอ จะลองซ้ำใน loop)
  if (!resolveBrokerViaMDNS()) {
    Serial.println("[WARN] ยังหา broker-mqtt.local ไม่เจอ จะพยายามต่อไปใน loop()");
  }
}

void loop() {
  mqtt_reconnect();
  client.loop();
  delay(50);
}
