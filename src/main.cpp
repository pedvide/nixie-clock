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
const uint8_t maxTubeBrightness = 200;
int8_t tubePWMLevel = averageTubeBrightness;

// status
enum class State {
  AWAKE = 0,
  POWERING_DOWN,
  SLEEPING,
  POWERING_UP,
  CATHODE_PREVENTION
};
const char *state_to_string[] = {"AWAKE", "POWERING_DOWN", "SLEEPING",
                                 "POWERING_UP", "CATHODE_PREVENTION"};
State current_state = State::AWAKE;
bool HVisOn = false;
uint8_t START_HOUR = 8;
uint8_t END_HOUR = 23;

#ifdef USE_TELNET_DEBUG
///// Command server
WiFiServer commandServer(23);
WiFiClient commandClient;
#define Serial commandClient
#endif

void switchHVOn() {
  digitalWrite(hvEnablePin, HIGH);
  HVisOn = true;
}

void switchHVOff() {
  digitalWrite(hvEnablePin, LOW);
  HVisOn = false;
}

bool isHVOn() { return HVisOn; }

void setTubeBrightness(uint8_t brightness) {
  if (brightness == 0) {
    switchHVOff();
  }

  if (brightness > maxTubeBrightness) {
    brightness = maxTubeBrightness;
  }
  analogWrite(anodePWMPin, brightness);
  tubePWMLevel = brightness;
}

uint8_t getTubeBrightness() { return tubePWMLevel; }

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

bool connect_to_time() {
  Serial.println("Connecting to time server");
  setDebug(ezDebugLevel_t::INFO);
  setServer("ntp.server.home");

  if (!Amsterdam.setCache(0)) {
    Amsterdam.setLocation("Europe/Berlin");
  }
  Amsterdam.setDefault();

  if (!waitForSync(30)) {
    setServer("pool.ntp.org");
    if (!waitForSync(30)) {
      return false;
    }
  }
  setInterval(60 * 60); // 1h in seconds

  Serial.println(
      "  Connection stablished with the time server. Using Amsterdam time.");
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
  const float brightnessMultiplier = 1.9;
  const uint8_t higherTubeBrightness =
      currentTubeBrightness > 134
          ? maxTubeBrightness
          : currentTubeBrightness * brightnessMultiplier;
  // Each iteration takes half_delay ms, due to the delay statements
  // (all other statements are much faster)
  // So iterate transitionTime_ms/(2*half_delay) times
  const uint8_t half_delay = 5;
  const uint8_t maxIterations = transitionTime_ms / (2 * half_delay);
  for (uint32_t i = 0; i < maxIterations; i++) {
    writeDigits(fromDigit1, fromDigit2, fromDigit3, fromDigit4);
    setTubeBrightness(
        map(maxIterations - i, 0, maxIterations, 1, higherTubeBrightness));
    delay(half_delay);
    writeDigits(toDigit1, toDigit2, toDigit3, toDigit4);
    setTubeBrightness(map(i, 0, maxIterations, 1, higherTubeBrightness));
    delay(half_delay);
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
  const uint8_t currentLevel = getTubeBrightness();
  if (!isHVOn()) {
    switchHVOn();
  }

  if (currentLevel >= maxTubeBrightness) {
    Serial.printf("\nTubes fully powered up\n> ");
    powerUpTubesTimer.stop();
  } else {
    setTubeBrightness(currentLevel + 1);
  }
}
Ticker powerUpTubesTimer(powerUpTubes, 400, 255, MILLIS);

void powerDownTubes() {
  const uint8_t currentLevel = getTubeBrightness();
  if (currentLevel == 0) {
    Serial.printf("\nTubes fully powered down\n> ");
    powerDownTubesTimer.stop();
    switchHVOff();
  } else {
    setTubeBrightness(currentLevel - 1);
  }
}
Ticker powerDownTubesTimer(powerDownTubes, 400, 255, MILLIS);

void randomNumbers() { transitionToNumber(random(9999), 500); }
Ticker preventCathodePoisoningTimer(randomNumbers, 5000, 120, MILLIS);

void setup_OTA() {
  ArduinoOTA.onStart([]() { Serial.println("Starting the OTA update."); });

  ArduinoOTA.onEnd([]() { Serial.println("Finished the OTA update."); });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static uint8_t last_perc_progress = 0;
    uint8_t perc_progress = (progress / (total / 100));
    if (((perc_progress % 10) == 0) && (perc_progress > last_perc_progress)) {
      digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
      writeNumber(perc_progress);
      Serial.printf("OTA progress: %u%%.\n", perc_progress);
      last_perc_progress = perc_progress;
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error (%u): ", error);
    writeNumber((uint8_t)error);
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

    delay(100);
  });

  ArduinoOTA.begin();
  Serial.println("OTA enabled.");
}

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
      commandClient = commandServer.accept(); // ready for new client
    } else {
      commandServer.accept().stop(); // have client, block new conections
      justConnected = true;
    }
  }

  if (commandClient && commandClient.connected()) {
    // On first connection
    if (justConnected) {
      Serial.println("Nixie tube clock (type help for commands)");
      Serial.print("> ");
    }
    justConnected = false;

    // client input processing
    while (commandClient.available()) {
      String command = commandClient.readStringUntil('\n');
      command.trim();
      // Serial.println(command);
      if (command == "hv on") {
        Serial.println("Switching HV on.");
        switchHVOn();
      } else if (command == "hv off") {
        Serial.println("Switching HV off.");
        switchHVOff();
      } else if ((command == "brightness") || (command == "br")) {
        Serial.printf("brightness: %d.\n", getTubeBrightness());
      } else if (command.startsWith("brightness") || command.startsWith("br")) {
        command.replace("brightness ", "");
        command.replace("br ", "");
        command.trim();
        int32_t new_brightness = command.toInt();
        if (new_brightness < 0) {
          new_brightness = 0;
        }
        if (new_brightness > maxTubeBrightness) {
          new_brightness = maxTubeBrightness;
        }
        Serial.printf("New brightness: %d.\n", new_brightness);
        setTubeBrightness(new_brightness);
      } else if (command.startsWith("start")) {
        command.replace("start ", "");
        command.trim();
        int32_t new_start_hour = command.toInt();
        if (new_start_hour < 0) {
          new_start_hour = 0;
        }
        if (new_start_hour > 23) {
          new_start_hour = 23;
        }
        Serial.printf("New start hour: %d.\n", new_start_hour);
        START_HOUR = new_start_hour;
      } else if (command.startsWith("end")) {
        command.replace("end ", "");
        command.trim();
        int32_t new_end_hour = command.toInt();
        if (new_end_hour < 0) {
          new_end_hour = 0;
        }
        if (new_end_hour > 23) {
          new_end_hour = 23;
        }
        Serial.printf("New end hour: %d.\n", new_end_hour);
        END_HOUR = new_end_hour;
      } else if (command == "time") {
        transitionToTime(Amsterdam.hour(), Amsterdam.minute());
      } else if (command == "cathode") {
        Serial.println("Running cathode poisoning prevention routine.");
        preventCathodePoisoningTimer.start();
      } else if (command == "cathode stop") {
        Serial.println("Stopping cathode poisoning prevention routine.");
        preventCathodePoisoningTimer.stop();
      } else if (command == "power up") {
        Serial.println("Powering tubes up.");
        powerUpTubesTimer.start();
      } else if (command == "power down") {
        Serial.println("Powering tubes down.");
        powerDownTubesTimer.start();
      } else if (command == "debug") {
        Serial.printf("current_state: %s.\n",
                      state_to_string[static_cast<int>(current_state)]);
        Serial.printf("HVisOn: %s.\n", HVisOn ? "true" : "false");
        Serial.printf("start hour: %d.\n", START_HOUR);
        Serial.printf("end hour: %d.\n", END_HOUR);
      } else if (command == "restart") {
        Serial.println("Restarting!");
        Serial.flush();
        delay(10);
        commandClient.stop();
        ESP.restart();
      } else {
        if (command != "help") {
          Serial.println("Command not recognized!");
        }
        Serial.println("Available commands: 'help', 'hv on', 'hv off', "
                       "'(br)ightness <0-255>', 'time', "
                       "'start <hour>', 'end <hour>',"
                       "'cathode', 'cathode stop', "
                       "'power down', 'power up', "
                       "'debug', 'restart'.");
      }
      Serial.print("> ");
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
#else
  commandServer.begin();
#endif

  connect_to_wifi();

  setup_OTA();

  connect_to_time();

  writeTime(Amsterdam.hour(), Amsterdam.minute());

  // switch HV source on
  pinMode(anodePWMPin, OUTPUT);
  pinMode(hvEnablePin, OUTPUT);
  delay(100);
  switchHVOn();
  setTubeBrightness(averageTubeBrightness);
  delay(100);

  // stop all timers to set their statuses
  powerUpTubesTimer.stop();
  powerDownTubesTimer.stop();
  preventCathodePoisoningTimer.stop();

  digitalWrite(LED_BUILTIN, HIGH); // end of setup
}

void awake() {
  // Only change display if the time has changed
  if (Amsterdam.minute() != lastMinute) {
    transitionToTime(Amsterdam.hour(), Amsterdam.minute());
    lastMinute = Amsterdam.minute();
  }

  if (Amsterdam.hour() == END_HOUR) {
    Serial.println("Powering down tubes for the night...");
    Serial.print("> ");
    powerDownTubesTimer.start();
    current_state = State::POWERING_DOWN;
  }
}

void powering_down() {
  if (powerDownTubesTimer.state() != RUNNING) {
    current_state = State::SLEEPING;
  }
}

void sleeping() {
  if (Amsterdam.hour() == START_HOUR) {
    Serial.println("Powering up tubes for the day...");
    Serial.print("> ");
    powerUpTubesTimer.start();
    current_state = State::POWERING_UP;
  }
}

void powering_up() {
  if (powerUpTubesTimer.state() != RUNNING) {
    Serial.println("Running cathode poisoning prevention routine.");
    Serial.print("> ");
    setTubeBrightness(255);
    preventCathodePoisoningTimer.start();
    current_state = State::CATHODE_PREVENTION;
  }
}

void cathode_prevention() {
  if (preventCathodePoisoningTimer.state() != RUNNING) {
    // After cathode routine, set tube brightness back to normal
    setTubeBrightness(averageTubeBrightness);
    Serial.println("Daily start routine finished.");
    Serial.print("> ");
    current_state = State::AWAKE;
  }
}

void loop() {
  // Update time library events
  events();
  powerDownTubesTimer.update();
  preventCathodePoisoningTimer.update();
  powerUpTubesTimer.update();

  // Deal with OTA
  ArduinoOTA.handle();

  // Handle user commands
#ifdef USE_TELNET_DEBUG
  handleCommands();
#endif

  switch (current_state) {
  case State::AWAKE:
    awake();
    break;
  case State::CATHODE_PREVENTION:
    cathode_prevention();
    break;
  case State::POWERING_DOWN:
    powering_down();
    break;
  case State::SLEEPING:
    sleeping();
    break;
  case State::POWERING_UP:
    powering_up();
    break;
  }
}
