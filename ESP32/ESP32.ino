#define IR_RECEIVE_PIN 15
#include <IRremote.hpp>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESP32Servo.h>
#include <TOTP.h>
#include <time.h>

const char* ssid          = "*****";
const char* wifiPassword  = "*****";

const char* ca_cert = R"EOF(
-----BEGIN CERTIFICATE-----
****************************
-----END CERTIFICATE-----
)EOF";

const char* mqtt_server = "ip";
const int   mqtt_port   = 8883;
const char* mqtt_user   = "esp32";
const char* mqtt_pass   = "123456";

const char* TOPIC_UNLOCK_CMD      = "home/lock/unlock/cmd";     
const char* TOPIC_SECURITY_CMD    = "home/lock/security/cmd";   
const char* TOPIC_TOTP_REFRESH    = "home/lock/totp/refresh";    

const char* TOPIC_LOCK_STATE      = "home/lock/state";          
const char* TOPIC_SECURITY_STATE  = "home/lock/security/state";  
const char* TOPIC_TOTP_CODE       = "home/lock/totp/code";      
const char* TOPIC_TOTP_EXPIRY     = "home/lock/totp/expiry";    
const char* TOPIC_ALERT           = "home/lock/alert";          
const char* TOPIC_STATUS          = "home/lock/status";         
const char* TOPIC_HEARTBEAT       = "home/lock/heartbeat";      

const int PIN_RED    = 25;
const int PIN_GREEN  = 26;
const int PIN_BUZZER = 27;
const int PIN_SERVO  = 13;

LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo lockServo;
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

const String lockPassword  = "123456";
const String adminPassword = "654321";

uint8_t totpSecret[] = { 0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x21, 0xDE, 0xAD, 0xBE, 0xEF };
TOTP totp(totpSecret, sizeof(totpSecret));

enum SecurityLevel { LOW_SEC = 0, MEDIUM_SEC = 1, HIGH_SEC = 2 };
SecurityLevel securityLevel = LOW_SEC;

String  enteredCode   = "";
int     failureCount  = 0;
bool    isLocked      = true;

String  activeTotpCode    = "";
unsigned long totpIssuedAt = 0;        
const unsigned long TOTP_VALID_MS = 30000; 

bool          lcdOverride        = false;
unsigned long lcdOverrideStart   = 0;
unsigned long lcdOverrideDuration = 0;
String        lcdLine0           = "";
String        lcdLine1           = "";

bool          servoUnlocked     = false;
bool          irSuppressed      = false;
unsigned long irSuppressTime    = 0;
const unsigned long IR_SUPPRESS_DURATION = 1000;
unsigned long servoUnlockTime   = 0;
const unsigned long RELOCK_DELAY = 5000;

bool          buzzerActive       = false;
unsigned long buzzerOnTime       = 0;
const unsigned long BUZZER_DURATION = 800;

bool          redLedActive       = false;
unsigned long redLedOnTime       = 0;
const unsigned long RED_LED_DURATION = 2000;

unsigned long lastWifiAttempt = 0;
void handleWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttempt > 5000) {
    lastWifiAttempt = millis();
    WiFi.begin(ssid, wifiPassword);
  }
}

unsigned long lastMqttAttempt = 0;
void handleMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;
  if (millis() - lastMqttAttempt > 2000) {
    lastMqttAttempt = millis();
    if (mqttClient.connect("ESP32_RemoteLock", mqtt_user, mqtt_pass, TOPIC_STATUS, 0, true, "OFFLINE")) {
      mqttClient.publish(TOPIC_STATUS, "ONLINE", true);
      mqttClient.subscribe(TOPIC_UNLOCK_CMD);
      mqttClient.subscribe(TOPIC_SECURITY_CMD);
      mqttClient.subscribe(TOPIC_TOTP_REFRESH);
      publishSecurityState();
      publishLockState();
    }
  }
}

void lcdPrint(String line0, String line1, unsigned long durationMs = 0) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line0.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line1.substring(0, 16));

  if (durationMs > 0) {
    lcdOverride         = true;
    lcdOverrideStart    = millis();
    lcdOverrideDuration = durationMs;
    lcdLine0            = line0;
    lcdLine1            = line1;
  }
}

void lcdShowIdle() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(isLocked ? "Status: LOCKED" : "Status: OPEN  ");
  lcd.setCursor(0, 1);
  String lvl = (securityLevel == LOW_SEC) ? "LOW" : (securityLevel == MEDIUM_SEC) ? "MED" : "HIGH";
  lcd.print("Sec:" + lvl + " pwd:");
}

void handleLcdOverride() {
  if (!lcdOverride) return;
  if (millis() - lcdOverrideStart >= lcdOverrideDuration) {
    lcdOverride = false;
    lcdShowIdle();
  }
}

void publishSecurityState() {
  const char* s = (securityLevel == LOW_SEC) ? "LOW" : (securityLevel == MEDIUM_SEC) ? "MEDIUM" : "HIGH";
  mqttClient.publish(TOPIC_SECURITY_STATE, s, true);
}

void publishLockState() {
  mqttClient.publish(TOPIC_LOCK_STATE, isLocked ? "LOCKED" : "UNLOCKED", true);
}

void publishAlert(const char* msg) {
  mqttClient.publish(TOPIC_ALERT, msg);
}

void generateAndPublishTOTP() {
  time_t now;
  time(&now);
  if (now < 1000000) {
    mqttClient.publish(TOPIC_TOTP_CODE, "NTP_WAIT");
    return;
  }
  char* code = totp.getCode(now);
  activeTotpCode  = String(code);
  totpIssuedAt    = millis();

  mqttClient.publish(TOPIC_TOTP_CODE, code);

  char expiryMsg[8];
  snprintf(expiryMsg, sizeof(expiryMsg), "30");
  mqttClient.publish(TOPIC_TOTP_EXPIRY, expiryMsg);
}

unsigned long lastTotpExpiryPublish = 0;
void taskTotpExpiry() {
  if (securityLevel != HIGH_SEC) return;
  if (activeTotpCode == "") return;
  if (millis() - lastTotpExpiryPublish < 1000) return;
  lastTotpExpiryPublish = millis();

  long elapsed = (millis() - totpIssuedAt) / 1000;
  long remaining = 30 - elapsed;
  if (remaining < 0) remaining = 0;

  char buf[8];
  snprintf(buf, sizeof(buf), "%ld", remaining);
  mqttClient.publish(TOPIC_TOTP_EXPIRY, buf);
}

void unlockAction() {
  isLocked       = false;
  servoUnlocked  = true;
  servoUnlockTime = millis();

  lockServo.write(90);
  irSuppressed   = true;
  irSuppressTime = millis();
  digitalWrite(PIN_GREEN, HIGH);
  digitalWrite(PIN_RED, LOW);
  failureCount = 0;

  lcdPrint("CORRECT!", "Unlocking...", 2000);
  publishLockState();
}

void relockAction() {
  servoUnlocked = false;
  isLocked      = true;

  lockServo.write(0);
  irSuppressed   = true;
  irSuppressTime = millis();
  digitalWrite(PIN_GREEN, LOW);

  lcdShowIdle();
  publishLockState();
}

void handleServoRelock() {
  if (!servoUnlocked) return;
  if (millis() - servoUnlockTime >= RELOCK_DELAY) {
    relockAction();
  }
}

void triggerBuzzer() {
  digitalWrite(PIN_BUZZER, HIGH);
  buzzerOnTime = millis();
  buzzerActive = true;
}

void handlePasswordEntry(String code) {
  if (code == lockPassword) {
    unlockAction();
  } else {
    failureCount++;
    digitalWrite(PIN_RED, HIGH);
    scheduleRedOff();
    lcdPrint("WRONG!", "Access Denied", 2000);

    if (failureCount >= 5) {
      lcdPrint("5 FAILURES!", "Lockout!", 3000);
      triggerBuzzer();
      failureCount = 0;
      publishAlert("LOCKOUT: 5 consecutive failures");
    }
  }
  enteredCode = "";
}

void handleTotpEntry(String code) {
  if (activeTotpCode == "") {
    lcdPrint("No TOTP Active", "Refresh first", 3000);
    enteredCode = "";
    return;
  }
  unsigned long elapsed = millis() - totpIssuedAt;
  if (elapsed > TOTP_VALID_MS) {
    lcdPrint("TOTP Expired!", "Refresh first", 3000);
    activeTotpCode = "";
    enteredCode    = "";
    return;
  }
  if (code == activeTotpCode) {
    activeTotpCode = "";
    unlockAction();
  } else {
    failureCount++;
    digitalWrite(PIN_RED, HIGH);
    scheduleRedOff();
    lcdPrint("WRONG TOTP!", "Try again", 2000);

    if (failureCount >= 5) {
      lcdPrint("5 FAILURES!", "Lockout!", 3000);
      triggerBuzzer();
      failureCount = 0;
      publishAlert("LOCKOUT: 5 consecutive failures");
    }
  }
  enteredCode = "";
}

void handleBuzzer() {
  if (!buzzerActive) return;
  if (millis() - buzzerOnTime >= BUZZER_DURATION) {
    digitalWrite(PIN_BUZZER, LOW);
    buzzerActive = false;
    digitalWrite(PIN_RED, LOW);
  }
}

void scheduleRedOff() {
  redLedActive = true;
  redLedOnTime = millis();
}

void handleRedLed() {
  if (!redLedActive) return;
  if (millis() - redLedOnTime >= RED_LED_DURATION) {
    if (!buzzerActive) digitalWrite(PIN_RED, LOW);
    redLedActive = false;
  }
}

String irDecodeDigit(uint32_t raw) {
  switch (raw) {
    case 0xE916FF00: return "0";
    case 0xF30CFF00: return "1";
    case 0xE718FF00: return "2";
    case 0xA15EFF00: return "3";
    case 0xF708FF00: return "4";
    case 0xE31CFF00: return "5";
    case 0xA55AFF00: return "6";
    case 0xBD42FF00: return "7";
    case 0xAD52FF00: return "8";
    case 0xB54AFF00: return "9";
    default:         return "";
  }
}

void handleIR() {
  if (irSuppressed && millis() - irSuppressTime >= IR_SUPPRESS_DURATION) {
    irSuppressed = false;
  }

  if (!IrReceiver.decode()) return;

  uint32_t raw = IrReceiver.decodedIRData.decodedRawData;
  IrReceiver.resume();

  if (irSuppressed) return;

  if (securityLevel == MEDIUM_SEC) {
    lcdPrint("Access Denied", "Med. Security", 2000);
    return;
  }

  String digit = irDecodeDigit(raw);
  if (digit == "") return; 

  enteredCode += digit;

  String masked = "";
  for (int i = 0; i < enteredCode.length(); i++) masked += "*";
  lcdPrint("Enter Code:", masked);

  if (enteredCode.length() >= 6) {
    if (securityLevel == LOW_SEC) {
      handlePasswordEntry(enteredCode);
    } else if (securityLevel == HIGH_SEC) {
      handleTotpEntry(enteredCode);
    }
  }
}

void applySecurityLevel(SecurityLevel newLevel) {
  securityLevel = newLevel;
  activeTotpCode = ""; 

  String levelName = (newLevel == LOW_SEC) ? "LOW" : (newLevel == MEDIUM_SEC) ? "MEDIUM" : "HIGH";

  lcdPrint("Security:", levelName, 4000);
  publishSecurityState();

  enteredCode = "";
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) return;

  String t = String(topic);

  if (t == TOPIC_SECURITY_CMD) {
    const char* pwd = doc["password"];
    if (!pwd) return;
    if (String(pwd) != adminPassword) {
      publishAlert("Wrong admin password attempt");
      return;
    }
    SecurityLevel next = (SecurityLevel)((securityLevel + 1) % 3);
    applySecurityLevel(next);
    return;
  }

  if (t == TOPIC_UNLOCK_CMD) {
    if (securityLevel == HIGH_SEC) return;
    if (securityLevel == MEDIUM_SEC) {
      const char* pwd = doc["password"];
      if (!pwd || String(pwd) != lockPassword) {
        publishAlert("Wrong lock password on dashboard");
        return;
      }
    }
    unlockAction();
    return;
  }

  if (t == TOPIC_TOTP_REFRESH) {
    if (securityLevel != HIGH_SEC) return;
    generateAndPublishTOTP();
    return;
  }
}

unsigned long tHeartbeat = 0;
void taskHeartbeat() {
  if (millis() - tHeartbeat < 5000) return;
  tHeartbeat = millis();

  char msg[64];
  snprintf(msg, sizeof(msg), "{\"uptime\":%lu,\"rssi\":%d}", millis() / 1000, WiFi.RSSI());
  mqttClient.publish(TOPIC_HEARTBEAT, msg);
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_RED,    OUTPUT);
  pinMode(PIN_GREEN,  OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_RED,    LOW);
  digitalWrite(PIN_GREEN,  LOW);
  digitalWrite(PIN_BUZZER, LOW);

  lockServo.attach(PIN_SERVO);
  lockServo.write(0);
  irSuppressed   = true;
  irSuppressTime = millis();

  lcd.init();
  lcd.backlight();
  lcdPrint("RemoteLock", "Booting...");

  IrReceiver.begin(IR_RECEIVE_PIN, ENABLE_LED_FEEDBACK);

  WiFi.begin(ssid, wifiPassword);
  Serial.print("Connecting WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
    lcdPrint("WiFi OK", WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi FAILED — offline mode");
    lcdPrint("WiFi FAILED", "Offline mode");
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  espClient.setCACert(ca_cert);
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);

  ArduinoOTA.setHostname("esp32-remotelock");
  ArduinoOTA.setPassword("ota1234");
  ArduinoOTA.begin();

  delay(1500);
  lcdShowIdle();
}

void loop() {
  ArduinoOTA.handle();
  handleWiFi();
  handleMQTT();
  mqttClient.loop();

  handleIR();
  handleServoRelock();
  handleBuzzer();
  handleRedLed();
  handleLcdOverride();
  taskTotpExpiry();
  taskHeartbeat();
}
