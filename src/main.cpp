#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <ezTime.h>

#include "config.h"

//// WiFi
const char *ssid PROGMEM = STASSID;
const char *password PROGMEM = STAPSK;
const char *hostname = "nixie-clock";

//// Time
Timezone Amsterdam;

const uint8_t latchPin = D3;
const uint8_t clockPin = D1;
const uint8_t dataPin = D2;
const uint8_t hvEnablePin = D6;
const uint8_t anodePWMPin = D0;

void connect_to_wifi() {
  Serial.println("Connecting to WiFi");

  WiFi.mode(WIFI_STA); // WiFi mode station (connect to wifi router only)
  WiFi.hostname(hostname);
  WiFi.begin(ssid, password);
  while (!WiFi.isConnected()) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    ESP.restart();
  }

  // Print ESP8266 Local IP Address
  Serial.printf("  Connected! IP: %s, hostname: %s.\n",
                WiFi.localIP().toString().c_str(), WiFi.hostname().c_str());
  Serial.printf("IP: %s, chip ID: %x.\n", WiFi.localIP().toString().c_str(),
                ESP.getChipId());
  WiFi.setAutoReconnect(true);
}

void setup_OTA() {
  ArduinoOTA.onStart([]() {
    digitalWrite(hvEnablePin, LOW); // HV off
    Serial.println("Starting the OTA update.");
  });

  ArduinoOTA.onEnd([]() { Serial.println("Finished the OTA update."); });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static uint8_t last_perc_progress = 0;
    uint8_t perc_progress = (progress / (total / 100));
    if (((perc_progress % 20) == 0) && (perc_progress > last_perc_progress)) {
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
      Serial.printf("OTA progress: %u%%.\n", perc_progress);
      last_perc_progress = perc_progress;
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error (%u): ", error);
    if (error == OTA_AUTH_ERROR)
      Serial.println(F("Auth Failed"));
    else if (error == OTA_BEGIN_ERROR)
      Serial.println(F("Begin Failed"));
    else if (error == OTA_CONNECT_ERROR)
      Serial.println(F("Connect Failed"));
    else if (error == OTA_RECEIVE_ERROR)
      Serial.println(F("Receive Failed"));
    else if (error == OTA_END_ERROR)
      Serial.println(F("End Failed"));
  });

  ArduinoOTA.begin();
  Serial.println("OTA enabled.");
}

bool connect_to_time() {
  Serial.println("Connecting to time server");
  setDebug(ezDebugLevel_t::INFO);
  setServer("ntp.server.home");

  if (!Amsterdam.setCache(0)) {
    Amsterdam.setLocation("Europe/Berlin");
  }
  Amsterdam.setDefault();

  if (!waitForSync(5)) {
    return false;
  }
  setInterval(60 * 60); // 1h in seconds

  Serial.println("  UTC: " + UTC.dateTime());
  Serial.println("  Amsterdam time: " + Amsterdam.dateTime());
  Serial.printf("Connection stablished with the time server (%s). Using "
                "Amsterdam time.\n",
                UTC.dateTime().c_str());
  Serial.printf("  Amsterdam time tube format: %d:%d.\n", Amsterdam.hour(),
                Amsterdam.minute());

  return true;
}

bool writeDigits(uint8_t digit1, uint8_t digit2, uint8_t digit3,
                 uint8_t digit4) {
  if ((digit1 > 9) | (digit2 > 9) | (digit3 > 9) | (digit4 > 9)) {
    return false;
  }
  // take the latchPin low so the tubes don't change while sending in bits
  digitalWrite(latchPin, LOW);

  // shift out the bits:
  shiftOut(dataPin, clockPin, MSBFIRST, digit3 | (digit4 << 4));
  shiftOut(dataPin, clockPin, MSBFIRST, digit1 | (digit2 << 4));

  // take the latch pin high so the tubes will change
  digitalWrite(latchPin, HIGH);

  return true;
}

bool writeTime(uint8_t hours, uint8_t minutes) {
  if ((hours > 23) | (minutes > 59)) {
    return false;
  }

  uint8_t digit4 = minutes % 10;
  uint8_t digit3 = int(minutes / 10) % 10;
  uint8_t digit2 = hours % 10;
  uint8_t digit1 = int(hours / 10) % 10;

  return writeDigits(digit1, digit2, digit3, digit4);
}

bool writeNumber(uint16_t number) {
  if (number > 9999) {
    return false;
  }

  uint8_t digit4 = number % 10;
  uint8_t digit3 = int(number / 10) % 10;
  uint8_t digit2 = int(number / 100) % 10;
  uint8_t digit1 = int(number / 1000) % 10;

  return writeDigits(digit1, digit2, digit3, digit4);
}

void setup() {

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW); // LED pin is active low

  // set pins to output so you can control the shift register
  pinMode(latchPin, OUTPUT);
  pinMode(clockPin, OUTPUT);
  pinMode(dataPin, OUTPUT);

  Serial.begin(115200);

  connect_to_wifi();

  setup_OTA();

  connect_to_time();

  writeTime(Amsterdam.hour(), Amsterdam.minute());

  // switch HV source on
  pinMode(anodePWMPin, OUTPUT);
  pinMode(hvEnablePin, OUTPUT);
  delay(20);
  analogWrite(anodePWMPin, 127);
  digitalWrite(hvEnablePin, HIGH);

  digitalWrite(LED_BUILTIN, HIGH); // end of setup
}

void loop() {
  // Update time if needed
  events();

  // Deal with OTA
  ArduinoOTA.handle();

  if (minuteChanged()) {
    Serial.printf("%d:%d\n", Amsterdam.hour(), Amsterdam.minute());
    writeTime(Amsterdam.hour(), Amsterdam.minute());
  }
}
