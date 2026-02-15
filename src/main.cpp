#include "ESPAsyncTCP.h"
#include "ESPAsyncWebServer.h"
#include "LittleFS.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <ezTime.h>

#include "config.h"

//// WiFi
const char *ssid PROGMEM = STASSID;
const char *password PROGMEM = STAPSK;
const char *hostname = "nixie-clock";

// EEPROM
// write after ezTime library cache
const uint16_t EEPROM_MAIN_START_ADDR = EEPROM_CACHE_LEN;
const uint16_t EEPROM_MAIN_LEN = 16;
const uint16_t EEPROM_MAIN_SIZE = EEPROM_MAIN_START_ADDR + EEPROM_MAIN_LEN;
const uint16_t EEPROM_MAIN_END_ADDR =
    EEPROM_MAIN_START_ADDR + EEPROM_MAIN_LEN - 1;

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
uint8_t averageTubeBrightness = 127;
const uint8_t maxTubeBrightness = 255;
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
typedef struct {
  uint8_t minute = 0;
  uint8_t hour;
} timePoint_t;
timePoint_t START_TIME = {.hour = 8};
timePoint_t END_TIME = {.hour = 23};

uint8_t checksum_eeprom_cache() {
  // Add all bytes in cache % 256 and add 42, that is the checksum written to
  // last byte. The 42 is because then checksum of all zeroes then isn't zero
  uint8_t checksum = 0;
  for (uint16_t addr = EEPROM_MAIN_START_ADDR; addr < EEPROM_MAIN_END_ADDR;
       addr++) {
    checksum += EEPROM.read(addr);
  }
  checksum += 42;
  return checksum;
}

bool read_stored_settings() {
  // for (int16_t addr = EEPROM_MAIN_START_ADDR; addr <= EEPROM_MAIN_END_ADDR;
  //      addr++) {
  //   Serial.printf("EEPROM[%d] = %d\n", addr, EEPROM.read(addr));
  // }
  uint8_t checksum = checksum_eeprom_cache();
  if (checksum != EEPROM.read(EEPROM_MAIN_END_ADDR)) {
    return false;
  }

  int16_t addr = EEPROM_MAIN_START_ADDR;

  averageTubeBrightness = EEPROM.read(addr++);
  START_TIME.hour = EEPROM.read(addr++);
  START_TIME.minute = EEPROM.read(addr++);
  END_TIME.hour = EEPROM.read(addr++);
  END_TIME.minute = EEPROM.read(addr++);

  Serial.println("Settings correctly read from cache");
  Serial.printf("Brightness: %d, START_TIME: %d:%d, END_TIME: %d:%d.\n",
                averageTubeBrightness, START_TIME.hour, START_TIME.minute,
                END_TIME.hour, END_TIME.minute);

  return true;
}

bool store_settings() {
  EEPROM.begin(EEPROM_MAIN_SIZE);

  uint16_t addr = EEPROM_MAIN_START_ADDR;

  // write settings
  EEPROM.write(addr++, averageTubeBrightness);
  EEPROM.write(addr++, START_TIME.hour);
  EEPROM.write(addr++, START_TIME.minute);
  EEPROM.write(addr++, END_TIME.hour);
  EEPROM.write(addr++, END_TIME.minute);

  // fill up the rest of the cache (except last byte) with 0s
  for (; addr < EEPROM_MAIN_END_ADDR; addr++) {
    EEPROM.write(addr, 0);
  }

  uint8_t checksum = checksum_eeprom_cache();
  // write checksum last
  EEPROM.write(EEPROM_MAIN_END_ADDR, checksum);

  if (EEPROM.commit()) {
    Serial.println("EEPROM successfully committed");
    return true;
  } else {
    Serial.println("ERROR! EEPROM commit failed");
    return false;
  }
}

bool isHVOn() { return HVisOn; }

void switchHVOn() {
  digitalWrite(hvEnablePin, HIGH);
  HVisOn = true;
}

void switchHVOff() {
  digitalWrite(hvEnablePin, LOW);
  HVisOn = false;
}

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

void powerUpTubes();
Ticker powerUpTubesTimer(powerUpTubes, 400, 255, MILLIS);
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

void powerDownTubes();
Ticker powerDownTubesTimer(powerDownTubes, 400, 255, MILLIS);
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

//// Web Server
AsyncWebServer web_server(80);
bool requested_restart = false;
const char web_server_html_header[] PROGMEM = R"=====(
<!DOCTYPE HTML>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="icon" href="data:,">
<link rel="stylesheet" type="text/css" href="style.css">
<script>
let data = %s;
</script>
<script src="script.js"></script>
<title>Nixie clock</title>
</head>
<body>
<header>
<h1>Nixie clock</h1>
<div class="button-bar">
  <div class="button-holder">
    <button class='button' id='btn-restart' onclick="fetch(`http://${data.hostname}/restart`)">Restart</button>
  </div>
  <div class="button-holder">
    <button class='button' id='btn-blink' onclick="fetch(`http://${data.hostname}/blink`)">Blink</button>
  </div>
</div>
</header>
<main>
<form>
  <label>
    Wake up time:
    <input id="start-time" type="time" name="start-time" required>
  </label>
  <label>
    Sleep time:
    <input id="end-time" type="time" name="end-time" required>
  </label>
  <div>
    <label class="hvToggle">
      <input type="checkbox" id="hvBtnToggle" name="hvBtnToggle">
      <span class="hvSlider"></span>
    </label>
    <label for="brightness"></label>
      <input type="range" id="brightness" name="brightness" min="0" max="255">
      <output id="brightness-value">116</output>
  </div>
</form>

Current Mode: <span id="currentMode"></span>.<br>
Digits: <span id="digit1"></span> <span id="digit2"></span> <span id="digit3"></span> <span id="digit4"></span>.<br>
Free RAM: <span id="freeRAM"></span> kB, largest contiguous: <span id="contiguousRAM"></span> kB (fragmentation: <span id="fragmentedRAM"></span>%%).<br>
</main>
<footer>
</footer>
</body>
</html>
)=====";

void setup_web_server() {
  Serial.print("Setting up web server...");

  // Web server
  web_server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("text/html");
    uint32_t hfree = 0;
    uint32_t hmax = 0;
    uint8_t hfrag = 0;
    ESP.getHeapStats(&hfree, &hmax, &hfrag);

    char json_buffer[1000];
    const char json_string[] = R"=====(
    {
    hostname: "%s",
    currentState: "%s",
    currentDigit1: %d,
    currentDigit2: %d,
    currentDigit3: %d,
    currentDigit4: %d,
    brightness: %d,
    isHVon: %d,
    startHour: %d,
    startMinute: %d,
    endHour: %d,
    endMinute: %d,
    hFree: %d,
    hMax: %d,
    hFrag: %d
    }
    )=====";

    snprintf(json_buffer, sizeof json_buffer, json_string, hostname,
             state_to_string[static_cast<int>(current_state)], currentDigit1,
             currentDigit2, currentDigit3, currentDigit4, getTubeBrightness(),
             isHVOn(), START_TIME.hour, START_TIME.minute, END_TIME.hour,
             END_TIME.minute, hfree / 1024, hmax / 1024, hfrag);

    response->printf_P(web_server_html_header, json_buffer);
    request->send(response);
  });

  web_server.serveStatic("/style.css", LittleFS, "/style.css")
      .setCacheControl("max-age=600");
  web_server.serveStatic("/script.js", LittleFS, "/script.js")
      .setCacheControl("max-age=600");

  web_server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request) {
    requested_restart = true;
    request->send(200, F("text/plain"), F("Ok"));
  });

  web_server.on("/blink", HTTP_GET, [](AsyncWebServerRequest *request) {
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
    request->send(200, F("text/plain"), F("Ok"));
  });

  web_server.on("/health", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, F("text/plain"), F("Ok"));
  });

  web_server.on("/settings", HTTP_POST, [](AsyncWebServerRequest *request) {
    bool settingsChanged = false;
    if (request->hasParam("startHour", true)) {
      AsyncWebParameter *p = request->getParam("startHour", true);
      const int readInt = p->value().toInt();
      if (START_TIME.hour != readInt) {
        START_TIME.hour = readInt;
        settingsChanged = true;
      }
    }
    if (request->hasParam("startMinute", true)) {
      AsyncWebParameter *p = request->getParam("startMinute", true);
      const int readInt = p->value().toInt();
      if (START_TIME.minute != readInt) {
        START_TIME.minute = readInt;
        settingsChanged = true;
      }
    }
    if (request->hasParam("endHour", true)) {
      AsyncWebParameter *p = request->getParam("endHour", true);
      const int readInt = p->value().toInt();
      if (END_TIME.hour != readInt) {
        END_TIME.hour = readInt;
        settingsChanged = true;
      }
    }
    if (request->hasParam("endMinute", true)) {
      AsyncWebParameter *p = request->getParam("endMinute", true);
      const int readInt = p->value().toInt();
      if (END_TIME.minute != readInt) {
        END_TIME.minute = readInt;
        settingsChanged = true;
      }
    }
    if (request->hasParam("brightness", true)) {
      AsyncWebParameter *p = request->getParam("brightness", true);
      const int readInt = p->value().toInt();
      if (getTubeBrightness() != readInt) {
        setTubeBrightness(readInt);
        // settingsChanged = true; // don't store brightness changes
      }
    }
    if (request->hasParam("isHVOn", true)) {
      AsyncWebParameter *p = request->getParam("isHVOn", true);
      if (p->value() == "true") {
        switchHVOn();
      } else {
        switchHVOff();
      }
    }

    if (settingsChanged) {
      // save settings to EEPROM
      if (store_settings()) {
        request->send(200, F("text/plain"), F("Settings stored to EEPROM"));
      } else {
        request->send(507, F("text/plain"),
                      F("Error storing settings to EEPROM"));
      }
    } else {
      request->send(200, F("text/plain"), F("Ok"));
    }
  });

  web_server.onNotFound([](AsyncWebServerRequest *request) {
    Serial.println(F("404."));
    request->send(404, F("text/plain"), F("Not found"));
  });

  // Start server
  web_server.begin();
  Serial.println("  done.");
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW); // LED pin is active low

  // set pins to output so you can control the shift register
  pinMode(latchPin, OUTPUT);
  pinMode(clockPin, OUTPUT);
  pinMode(dataPin, OUTPUT);

  Serial.begin(115200);
  EEPROM.begin(EEPROM_MAIN_SIZE);

  LittleFS.begin();

  Serial.printf("Last restart due to %s.\n", ESP.getResetReason().c_str());
  Serial.printf("CPU freq: %d MHz, Flash size: %d kB, Sketch size: %d kB "
                "(free: %d kB), Free RAM: %d kB.\n",
                ESP.getCpuFreqMHz(), ESP.getFlashChipRealSize() / 1024,
                ESP.getSketchSize() / 1024, ESP.getFreeSketchSpace() / 1024,
                ESP.getFreeHeap() / 1024);

  if (!read_stored_settings()) {
    Serial.print("Invalid data in EEPROM cache. Writing default settings...");
    store_settings();
    Serial.println(" done!");
  }
  EEPROM.end();

  connect_to_wifi();

  setup_web_server();

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

  if (((Amsterdam.hour() < START_TIME.hour) |
       (Amsterdam.hour() >= END_TIME.hour)) &
      ((Amsterdam.minute() < START_TIME.minute) |
       (Amsterdam.minute() >= END_TIME.minute))) {
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
  if (((Amsterdam.hour() >= START_TIME.hour) |
       (Amsterdam.hour() < END_TIME.hour)) &
      ((Amsterdam.minute() >= START_TIME.minute) |
       (Amsterdam.minute() < END_TIME.minute))) {
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

  if (requested_restart) {
    ESP.restart();
  }

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
