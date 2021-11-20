#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <ezTime.h>

#include "config.h"

//// WiFi
const char *ssid PROGMEM = STASSID;
const char *password PROGMEM = STAPSK;
const char *hostname = "nixie-clock";

//// Time
Timezone Amsterdam;
uint8_t lastMinute = 61; // sigil value

// Pins
const uint8_t latchPin = D3;
const uint8_t clockPin = D1;
const uint8_t dataPin = D2;
const uint8_t hvEnablePin = D6;
const uint8_t anodePWMPin = D0;

int8_t tubePWMLevel = 127;

#ifdef USE_TELNET_DEBUG
///// Telnet
// ansi stuff, could always use printf instead of concat
String ansiPRE = "\033";      // escape code
String ansiHOME = "\033[H";   // cursor home
String ansiESC = "\033[2J";   // esc
String ansiCLC = "\033[?25l"; // invisible cursor

String ansiEND = "\033[0m"; // closing tag for styles
String ansiBOLD = "\033[1m";

String ansiRED = "\033[41m"; // red background
String ansiGRN = "\033[42m"; // green background
String ansiBLU = "\033[44m"; // blue background

String ansiREDF = "\033[31m"; // red foreground
String ansiGRNF = "\033[34m"; // green foreground
String ansiBLUF = "\033[32m"; // blue foreground
String BELL = "\a";

// declare telnet server (do NOT put in setup())
WiFiServer TelnetServer(23);
WiFiClient TelnetClient;
#define Serial TelnetClient
#endif

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
  Serial.printf("  Amsterdam time tube format: %02d:%02d.\n", Amsterdam.hour(),
                Amsterdam.minute());

  return true;
}

void handleTelnet() {
  if (TelnetServer.hasClient()) {
    // client is connected
    if (!TelnetClient || !TelnetClient.connected()) {
      if (TelnetClient) {
        TelnetClient.stop(); // client disconnected
      }
      TelnetClient = TelnetServer.available(); // ready for new client
    } else {
      TelnetServer.available().stop(); // have client, block new conections
    }
  }

  if (TelnetClient && TelnetClient.connected() && TelnetClient.available()) {
    // client input processing
    while (TelnetClient.available()) {
      Serial.write(TelnetClient.read());
    }
  }
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

void powerDownTubes() {
  tubePWMLevel -= 1;
  if (tubePWMLevel <= 0) {
    tubePWMLevel = 0;
    digitalWrite(anodePWMPin, LOW);
  } else {
    analogWrite(anodePWMPin, tubePWMLevel);
  }
}
Ticker powerDownTubesTimer(powerDownTubes, 100, 0, MILLIS);

void setup() {

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW); // LED pin is active low

  // set pins to output so you can control the shift register
  pinMode(latchPin, OUTPUT);
  pinMode(clockPin, OUTPUT);
  pinMode(dataPin, OUTPUT);

#ifndef USE_TELNET_DEBUG
  Serial.begin(115200);
#endif

  connect_to_wifi();

  setup_OTA();

#ifdef USE_TELNET_DEBUG
  TelnetServer.begin();
#endif

  connect_to_time();

  writeTime(Amsterdam.hour(), Amsterdam.minute());

  // switch HV source on
  pinMode(anodePWMPin, OUTPUT);
  pinMode(hvEnablePin, OUTPUT);
  delay(20);
  analogWrite(anodePWMPin, tubePWMLevel);
  digitalWrite(hvEnablePin, HIGH);

  digitalWrite(LED_BUILTIN, HIGH); // end of setup
}

void loop() {
  // Update time if needed
  events();

  // Deal with OTA
  ArduinoOTA.handle();

  // Only change display if the time has changed
  if (Amsterdam.minute() != lastMinute) {
    Serial.printf("%02d:%02d\n", Amsterdam.hour(), Amsterdam.minute());
    writeTime(Amsterdam.hour(), Amsterdam.minute());
    lastMinute = Amsterdam.minute();
  }

  if ((Amsterdam.hour() >= 0) && (Amsterdam.hour() <= 8)) {
    if ((powerDownTubesTimer.state() != RUNNING) && (tubePWMLevel > 0)) {
      Serial.println("Powering down tubes for the night...");
      powerDownTubesTimer.start();
    }
    if ((powerDownTubesTimer.state() != STOPPED) && (tubePWMLevel == 0)) {
      Serial.println("Tubes fully powered down.");
      powerDownTubesTimer.stop();
      digitalWrite(hvEnablePin, LOW);
    }
    powerDownTubesTimer.update();
  } else {
    if (tubePWMLevel == 0) {
      Serial.println("Powering up tubes for the day.");
      tubePWMLevel = 127;
      digitalWrite(hvEnablePin, HIGH);
      analogWrite(anodePWMPin, tubePWMLevel);
    }
  }

#ifdef USE_TELNET_DEBUG
  handleTelnet();
#endif
}
