#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_NeoPixel.h>

// PINI
#define PIN_LDR     34
#define PIN_PIR     27
#define PIN_BTN     14
#define PIN_NEOPIX  26
#define NPIXELS     16

// WIFI / MQTT
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

const char* MQTT_HOST = "test.mosquitto.org"; // Mosquitto test broker public
const int   MQTT_PORT = 1883;

const char* TOPIC_TELEM  = "casa/lumina/telemetrie";
const char* TOPIC_STATUS = "casa/lumina/status";
const char* TOPIC_CMD    = "casa/lumina/comanda";

// PARAMETRI
int LDR_THRESHOLD_DARK = 1800;           // prag intuneric (0..4095)
unsigned long MOTION_TIMEOUT_MS = 8000;  // mentine lumina aprinsa X ms dupa miscare
unsigned long MOTION_PULSE_MS = 250;     // flash galben scurt la inceput de miscare

int BRIGHTNESS_MANUAL = 180;             // mod ON luminozitate (0..255)
int BRIGHTNESS_MIN_AUTO = 50;
int BRIGHTNESS_MAX_AUTO = 220;

int FADE_STEP = 6;
unsigned long BTN_DEBOUNCE_MS = 60;

// MOD
enum Mode { MODE_AUTO, MODE_ON, MODE_OFF };
Mode mode = MODE_AUTO;

const char* modeToStr(Mode m) {
  if (m == MODE_AUTO) return "AUTO";
  if (m == MODE_ON)   return "ON";
  return "OFF";
}

// STARE
int ldrValue = 0;
bool motionNow = false;
bool prevMotion = false;

unsigned long lastMotionMs = 0;
unsigned long lastMotionPulseMs = 0;

bool lastBtnStable = HIGH;
bool lastBtnReading = HIGH;
unsigned long lastBtnChangeMs = 0;

int currentBrightness = 0;
int targetBrightness  = 0;

// NEOPIXEL
Adafruit_NeoPixel pixels(NPIXELS, PIN_NEOPIX, NEO_GRB + NEO_KHZ800);

// Culori
uint32_t colorWarmWhite() { return pixels.Color(255, 180, 120); } // lumina ambientala 
uint32_t colorBlue()      { return pixels.Color(  0, 120, 255); } // AUTO 
uint32_t colorGreen()     { return pixels.Color(  0, 255,  60); } // ON (manual activ)
uint32_t colorRed()       { return pixels.Color(255,   0,   0); } // OFF (dezactivat)
uint32_t colorYellow()    { return pixels.Color(255, 180,   0); } // eveniment miscare

WiFiClient espClient;
PubSubClient mqtt(espClient);

// AUXILIARE
int autoBrightnessFromLdr(int ldr) {
  int t = LDR_THRESHOLD_DARK;
  if (ldr >= t) return BRIGHTNESS_MIN_AUTO;
  if (ldr <= 200) return BRIGHTNESS_MAX_AUTO;

  long b = BRIGHTNESS_MAX_AUTO
          - ((long)(ldr - 200) * (BRIGHTNESS_MAX_AUTO - BRIGHTNESS_MIN_AUTO)) / (t - 200);

  if (b < BRIGHTNESS_MIN_AUTO) b = BRIGHTNESS_MIN_AUTO;
  if (b > BRIGHTNESS_MAX_AUTO) b = BRIGHTNESS_MAX_AUTO;
  return (int)b;
}

void setAllPixels(uint32_t c) {
  for (int i = 0; i < NPIXELS; i++) pixels.setPixelColor(i, c);
  pixels.show();
}

// SENZORI
void readSensors() {
  ldrValue = analogRead(PIN_LDR);

  bool motionReading = (digitalRead(PIN_PIR) == HIGH);
  motionNow = motionReading;

  // LOW -> HIGH = inceput miscare (declanseaza o singura data flash-ul)
  if (!prevMotion && motionReading) {
    lastMotionPulseMs = millis();
    lastMotionMs = millis(); // pornim timer-ul
  }

  // daca miscarea ramane crescuta, timerul se "reincarca"
  // ca sa tina lumina aprinsa cat timp e miscare si apoi inca MOTION_TIMEOUT_MS dupa.
  if (motionReading) {
    lastMotionMs = millis();
  }

  prevMotion = motionReading;
}

void handleButton() {
  bool reading = digitalRead(PIN_BTN);

  if (reading != lastBtnReading) {
    lastBtnReading = reading;
    lastBtnChangeMs = millis();
  }

  if (millis() - lastBtnChangeMs > BTN_DEBOUNCE_MS) {
    if (lastBtnStable == HIGH && reading == LOW) {
      // apasare: AUTO -> ON -> OFF -> AUTO
      if (mode == MODE_AUTO) mode = MODE_ON;
      else if (mode == MODE_ON) mode = MODE_OFF;
      else mode = MODE_AUTO;
    }
    lastBtnStable = reading;
  }
}

// DECIZIE
bool computeLightOn() {
  bool esteIntuneric = (ldrValue < LDR_THRESHOLD_DARK);
  bool miscareRecenta = (millis() - lastMotionMs) < MOTION_TIMEOUT_MS;

  if (mode == MODE_ON) return true;
  if (mode == MODE_OFF) return false;

  // AUTO: aprinde doar daca e intuneric + miscare recenta
  return (esteIntuneric && miscareRecenta);
}

// VIZUAL
void updateVisuals(bool lightOn) {
  bool pulse = (millis() - lastMotionPulseMs) < MOTION_PULSE_MS;

  // Target luminozitate
  if (lightOn) {
    if (mode == MODE_AUTO) targetBrightness = autoBrightnessFromLdr(ldrValue);
    else targetBrightness = BRIGHTNESS_MANUAL;
  } else {
    targetBrightness = 35; // mod indicator dim cand lampa e stinsa
  }

  if (currentBrightness < targetBrightness) currentBrightness += FADE_STEP;
  else if (currentBrightness > targetBrightness) currentBrightness -= FADE_STEP;

  if (currentBrightness < 0) currentBrightness = 0;
  if (currentBrightness > 255) currentBrightness = 255;

  pixels.setBrightness(currentBrightness);

  // Mod color (cand e OFF)
  uint32_t modeColor = colorBlue();
  if (mode == MODE_ON) modeColor = colorGreen();
  else if (mode == MODE_OFF) modeColor = colorRed();

  if (pulse) {
    for (int i = 0; i < NPIXELS; i++) pixels.setPixelColor(i, colorYellow());
  } else {
    if (lightOn) {
      for (int i = 0; i < NPIXELS; i++) pixels.setPixelColor(i, colorWarmWhite());
    } else {
      for (int i = 0; i < NPIXELS; i++) pixels.setPixelColor(i, modeColor);
    }
  }

  pixels.show();
}

// MQTT conectare
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  // Accept:
  // {"mode":"AUTO"} / {"mode":"ON"} / {"mode":"OFF"}
  // {"timeout":8000}  (ms)
  // {"prag":1800}     (LDR threshold)
  if (msg.indexOf("\"mode\"") >= 0) {
    if (msg.indexOf("AUTO") >= 0) mode = MODE_AUTO;
    else if (msg.indexOf("ON") >= 0) mode = MODE_ON;
    else if (msg.indexOf("OFF") >= 0) mode = MODE_OFF;
  }

  int tPos = msg.indexOf("\"timeout\"");
  if (tPos >= 0) {
    int colon = msg.indexOf(':', tPos);
    if (colon >= 0) {
      unsigned long v = (unsigned long) msg.substring(colon + 1).toInt();
      if (v >= 1000 && v <= 300000) MOTION_TIMEOUT_MS = v; // 1s..5min
    }
  }

  int pPos = msg.indexOf("\"prag\"");
  if (pPos >= 0) {
    int colon = msg.indexOf(':', pPos);
    if (colon >= 0) {
      int v = msg.substring(colon + 1).toInt();
      if (v >= 0 && v <= 4095) LDR_THRESHOLD_DARK = v;
    }
  }

  // Status actualizat
  String status = String("{\"mod\":\"") + modeToStr(mode) +
                  String("\",\"prag\":") + LDR_THRESHOLD_DARK +
                  String(",\"timeout_ms\":") + MOTION_TIMEOUT_MS + String("}");
  mqtt.publish(TOPIC_STATUS, status.c_str(), true);
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Conectare WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectat.");
}

void connectMqtt() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  Serial.print("Conectare MQTT la broker Mosquitto public (test.mosquitto.org)...");
  while (!mqtt.connected()) {
    String clientId = String("esp32-lumina-") + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqtt.connect(clientId.c_str())) {
      Serial.println(" OK");
      mqtt.subscribe(TOPIC_CMD);

      // status initial pastrat
      String status = String("{\"mod\":\"") + modeToStr(mode) +
                      String("\",\"prag\":") + LDR_THRESHOLD_DARK +
                      String(",\"timeout_ms\":") + MOTION_TIMEOUT_MS + String("}");
      mqtt.publish(TOPIC_STATUS, status.c_str(), true);
    } else {
      Serial.print(" esec, retry in 1s. Cod=");
      Serial.println(mqtt.state());
      delay(1000);
    }
  }
}

// TELEMETRIE
void publishTelemetry(bool lightOn) {
  // JSON simplu pentru DB ulterior
  String telem = "{";
  telem += "\"ldr\":" + String(ldrValue) + ",";
  telem += "\"pir\":" + String(motionNow ? 1 : 0) + ",";
  telem += "\"intuneric\":" + String((ldrValue < LDR_THRESHOLD_DARK) ? 1 : 0) + ",";
  telem += "\"mod\":\"" + String(modeToStr(mode)) + "\",";
  telem += "\"lampa\":\"" + String(lightOn ? "APRINSA" : "STINSA") + "\",";
  telem += "\"timeout_ms\":" + String(MOTION_TIMEOUT_MS);
  telem += "}";

  mqtt.publish(TOPIC_TELEM, telem.c_str(), false);
}

// SETUP / LOOP
void setup() {
  Serial.begin(115200);

  pinMode(PIN_PIR, INPUT);
  pinMode(PIN_BTN, INPUT_PULLUP);

  pixels.begin();
  pixels.clear();
  pixels.show();

  connectWiFi();
  connectMqtt();

  Serial.println("Sistem iluminat inteligent pornit.");
  Serial.println("Buton: AUTO -> ON -> OFF. MQTT comenzi pe casa/lumina/comanda");
}

void loop() {
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  readSensors();
  handleButton();

  bool lightOn = computeLightOn();
  updateVisuals(lightOn);

  static unsigned long lastPrint = 0;
  static unsigned long lastTelem = 0;

  if (millis() - lastPrint > 1000) {
    lastPrint = millis();
    bool esteIntuneric = (ldrValue < LDR_THRESHOLD_DARK);

    Serial.printf("Lumina=%s | Mod=%s | Intuneric=%s | PIR=%s | LDR=%d | Timeout=%lums\n",
                  lightOn ? "APRINSA" : "STINSA",
                  modeToStr(mode),
                  esteIntuneric ? "DA" : "NU",
                  motionNow ? "DA" : "NU",
                  ldrValue,
                  MOTION_TIMEOUT_MS);
  }

  if (millis() - lastTelem > 3000) {
    lastTelem = millis();
    publishTelemetry(lightOn);
  }

  delay(20);
}
