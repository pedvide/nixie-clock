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

// Tube digits
uint8_t currentDigit1, currentDigit2, currentDigit3, currentDigit4;

// Brightness
const uint8_t averageTubeBrightness = 127;
int8_t tubePWMLevel = averageTubeBrightness;

#ifdef USE_TELNET_DEBUG
///// Command server
WiFiServer commandServer(23);
WiFiClient commandClient;
#define Serial commandClient
#endif

void switchHVOn() { digitalWrite(hvEnablePin, HIGH); }

void switchHVOff() { digitalWrite(hvEnablePin, LOW); }

void setTubeBrightness(uint8_t brightness) {
  if (brightness == 0) {
    digitalWrite(anodePWMPin, LOW);
  } else if (brightness == 255) {
    switchHVOn();
    digitalWrite(anodePWMPin, HIGH);
  } else {
    switchHVOn();
    analogWrite(anodePWMPin, brightness);
  }
  tubePWMLevel = brightness;
}

int8_t getTubeBrightness() { return tubePWMLevel; }

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
    switchHVOff();
    Serial.println("Starting the OTA update.");
#ifdef USE_TELNET_DEBUG
    commandClient.stop();
#endif
  });

  ArduinoOTA.onEnd([]() { Serial.println("Finished the OTA update."); });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static uint8_t last_perc_progress = 0;
    uint8_t perc_progress = (progress / (total / 100));
    if (((perc_progress % 10) == 0) && (perc_progress > last_perc_progress)) {
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

  Serial.printf("  Connection stablished with the time server (%s). Using "
                "Amsterdam time.\n",
                UTC.dateTime().c_str());
  Serial.println("  UTC: " + UTC.dateTime());
  Serial.println("  Amsterdam time: " + Amsterdam.dateTime());

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

  currentDigit1 = digit1;
  currentDigit2 = digit2;
  currentDigit3 = digit3;
  currentDigit4 = digit4;

  return true;
}

bool writeTime(uint8_t hours, uint8_t minutes) {
  if ((hours > 23) | (minutes > 59)) {
    return false;
  }
  Serial.printf("\nTime changed to %02d:%02d.\n", hours, minutes);
  Serial.print("> ");

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

bool transitionToDigits(uint8_t toDigit1, uint8_t toDigit2, uint8_t toDigit3,
                        uint8_t toDigit4, uint16_t transitionTime_ms = 1000) {
  if ((toDigit1 > 9) | (toDigit2 > 9) | (toDigit3 > 9) | (toDigit4 > 9)) {
    return false;
  }

  const uint8_t fromDigit1 = currentDigit1;
  const uint8_t fromDigit2 = currentDigit2;
  const uint8_t fromDigit3 = currentDigit3;
  const uint8_t fromDigit4 = currentDigit4;

  const uint8_t currentTubeBrightness = getTubeBrightness();
  const uint8_t maxIterations = transitionTime_ms / 10;
  for (uint32_t i = 0; i < maxIterations; i++) {
    writeDigits(fromDigit1, fromDigit2, fromDigit3, fromDigit4);
    setTubeBrightness(map(maxIterations - i, 0, maxIterations, 1,
                          currentTubeBrightness * 1.9));
    delay(5);
    writeDigits(toDigit1, toDigit2, toDigit3, toDigit4);
    setTubeBrightness(map(i, 0, maxIterations, 1, currentTubeBrightness * 1.9));
    delay(5);
  }

  setTubeBrightness(currentTubeBrightness);
  return writeDigits(toDigit1, toDigit2, toDigit3, toDigit4);
}

bool transitionToNumber(uint16_t toNumber, uint16_t transitionTime_ms = 1000) {
  if (toNumber > 9999) {
    return false;
  }

  uint8_t toDigit4 = toNumber % 10;
  uint8_t toDigit3 = int(toNumber / 10) % 10;
  uint8_t toDigit2 = int(toNumber / 100) % 10;
  uint8_t toDigit1 = int(toNumber / 1000) % 10;

  return transitionToDigits(toDigit1, toDigit2, toDigit3, toDigit4,
                            transitionTime_ms);
}

bool transitionToTime(uint8_t toHours, uint8_t toMinutes,
                      uint16_t transitionTime_ms = 1000) {
  if ((toHours > 23) | (toMinutes > 59)) {
    return false;
  }
  Serial.printf("\nTime changed to %02d:%02d.\n", toHours, toMinutes);
  Serial.print("> ");

  uint8_t toDigit4 = toMinutes % 10;
  uint8_t toDigit3 = int(toMinutes / 10) % 10;
  uint8_t toDigit2 = toHours % 10;
  uint8_t toDigit1 = int(toHours / 10) % 10;

  return transitionToDigits(toDigit1, toDigit2, toDigit3, toDigit4,
                            transitionTime_ms);
}

void powerUpTubes() {
  tubePWMLevel++;
  if (tubePWMLevel > averageTubeBrightness) {
    tubePWMLevel = averageTubeBrightness;
  }
  setTubeBrightness(tubePWMLevel);
}
Ticker powerUpTubesTimer(powerUpTubes, 500, 127, MILLIS);

void powerDownTubes() {
  tubePWMLevel--;
  if (tubePWMLevel < 0) {
    tubePWMLevel = 0;
  }
  setTubeBrightness(tubePWMLevel);
}
Ticker powerDownTubesTimer(powerDownTubes, 500, 255, MILLIS);

void preventCathodePoisoning() {
  // Wait for power up to finish
  if (powerUpTubesTimer.state() != RUNNING) {
    // transitionToNumber(random(9999), 350);
    transitionToDigits((currentDigit1 + 1) % 10, (currentDigit2 + 1) % 10,
                       (currentDigit3 + 1) % 10, (currentDigit4 + 1) % 10, 400);
  }
}
Ticker preventCathodePoisoningTimer(preventCathodePoisoning, 500, 2000, MILLIS);

void rollRight() {
  transitionToDigits(currentDigit4, currentDigit1, currentDigit2, currentDigit3,
                     500);
}
Ticker rollRightTimer(rollRight, 800, 100, MILLIS);

#ifdef USE_TELNET_DEBUG
bool justConnected = true;
void handleCommands() {
  if (commandServer.hasClient()) {
    // client is connected
    if (!commandClient || !commandClient.connected()) {
      if (commandClient) {
        commandClient.stop(); // client disconnected
        justConnected = true;
      }
      commandClient = commandServer.available(); // ready for new client
    } else {
      commandServer.available().stop(); // have client, block new conections
      justConnected = true;
    }
  }

  if (commandClient && commandClient.connected()) {
    // On first connection
    if (justConnected) {
      Serial.println("Nixie tube clock");
      Serial.print("> ");
    }
    justConnected = false;

    // client input processing
    while (commandClient.available()) {
      String command = commandClient.readStringUntil('\n');
      // Serial.println(command);
      if (command == "hv on") {
        Serial.println("Switching HV on.");
        Serial.print("> ");
        switchHVOn();
      } else if (command == "hv off") {
        Serial.println("Switching HV off.");
        Serial.print("> ");
        switchHVOff();
      } else if (command.startsWith("brightness") || command.startsWith("br")) {
        command.replace("brightness ", "");
        command.replace("br ", "");
        command.trim();
        int32_t new_brightness = command.toInt();
        if (new_brightness < 0) {
          new_brightness = 0;
        }
        if (new_brightness > 255) {
          new_brightness = 255;
        }
        Serial.printf("New brightness: %d.\n", new_brightness);
        Serial.print("> ");
        setTubeBrightness(new_brightness);
      } else if (command == "time") {
        transitionToTime(Amsterdam.hour(), Amsterdam.minute());
      } else if (command == "random") {
        Serial.println("Running cathode poisoning prevention routine.");
        Serial.print("> ");
        preventCathodePoisoningTimer.start();
      } else if (command == "random stop") {
        Serial.println("Stopping cathode poisoning prevention routine.");
        Serial.print("> ");
        preventCathodePoisoningTimer.stop();
      } else if (command == "roll") {
        Serial.println("Rolling right.");
        Serial.print("> ");
        rollRightTimer.start();
      } else if (command == "roll stop") {
        Serial.println("Stopping rolling right.");
        Serial.print("> ");
        rollRightTimer.stop();
      } else if (command == "power up") {
        Serial.println("Powering tubes up.");
        Serial.print("> ");
        powerUpTubesTimer.start();
      } else if (command == "power down") {
        Serial.println("Powering tubes down.");
        Serial.print("> ");
        powerDownTubesTimer.start();
      } else if (command == "restart") {
        Serial.println("Restarting!");
        Serial.flush();
        delay(10);
        commandClient.stop();
        ESP.restart();
      } else if (command == "") {
        Serial.print("> ");
      } else {
        Serial.println("Command not recognized!");
        Serial.println("Available commands: 'hv on', 'hv off', "
                       "'(br)ightness <0-255>', 'time', "
                       "'random', 'random stop', "
                       "'power down', 'power up', 'restart'.");
        Serial.print("> ");
      }
    }
  }
}
#endif

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

#ifdef USE_TELNET_DEBUG
  commandServer.begin();
#endif

  setup_OTA();

  connect_to_time();

  writeTime(Amsterdam.hour(), Amsterdam.minute());

  // switch HV source on
  pinMode(anodePWMPin, OUTPUT);
  pinMode(hvEnablePin, OUTPUT);
  delay(20);
  setTubeBrightness(averageTubeBrightness);

  digitalWrite(LED_BUILTIN, HIGH); // end of setup
}

void loop() {
  // Update time library events
  events();

  // Deal with OTA
  ArduinoOTA.handle();

  // Handle user commands
#ifdef USE_TELNET_DEBUG
  handleCommands();
#endif

  // Switch tubes on for the day and
  // run the cathode poisoning prevention routine
  if ((Amsterdam.hour() == 8) && (Amsterdam.minute() == 0)) {
    if (powerUpTubesTimer.state() != RUNNING) {
      Serial.println("Powering up tubes for the night...");
      Serial.print("> ");
      powerUpTubesTimer.start();
    }

    if (preventCathodePoisoningTimer.state() != RUNNING) {
      Serial.println("Running cathode poisoning prevention routine.");
      Serial.print("> ");
      preventCathodePoisoningTimer.start();
    }
  }
  powerUpTubesTimer.update();
  preventCathodePoisoningTimer.update();

  // Switch tubes off for the night at midnight
  if ((Amsterdam.hour() == 0) && (Amsterdam.minute() == 0)) {
    if (powerDownTubesTimer.state() != RUNNING) {
      Serial.println("Powering down tubes for the night...");
      Serial.print("> ");
      powerDownTubesTimer.start();
    }
  }
  powerDownTubesTimer.update();

  // Only runs on command
  rollRightTimer.update();

  // Day tasks
  if ((Amsterdam.hour() >= 8) &&
      (preventCathodePoisoningTimer.state() != RUNNING)) {
    // Only change display if the time has changed
    if (Amsterdam.minute() != lastMinute) {
      transitionToTime(Amsterdam.hour(), Amsterdam.minute());
      lastMinute = Amsterdam.minute();
    }
  }
}
