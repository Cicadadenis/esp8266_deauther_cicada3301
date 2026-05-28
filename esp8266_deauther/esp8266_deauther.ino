/* =====================
   This software is licensed under the MIT License:
   https://github.com/spacehuhntech/esp8266_deauther
   ===================== */

extern "C" {
    // Please follow this tutorial:
    // https://github.com/spacehuhn/esp8266_deauther/wiki/Installation#compiling-using-arduino-ide
    // And be sure to have the right board selected
  #include "user_interface.h"
}

#include "EEPROMHelper.h"

#include "src/ArduinoJson-v5.13.5/ArduinoJson.h"
#if ARDUINOJSON_VERSION_MAJOR != 5
// The software was build using ArduinoJson v5.x
// version 6 is still in beta at the time of writing
// go to tools -> manage libraries, search for ArduinoJSON and install version 5
#error Please upgrade/downgrade ArduinoJSON library to version 5!
#endif // if ARDUINOJSON_VERSION_MAJOR != 5

#include "oui.h"
#include "language.h"
#include "functions.h"
#include "settings.h"
#include "Names.h"
#include "SSIDs.h"
#include "Scan.h"
#include "Attack.h"
#include "HandshakeCapture.h"
#include "client_tracker.h"
#include "logger.h"
#include "CLI.h"
#include "A_config.h"
#if USE_DISPLAY
#include "DisplayUI.h"
#endif

#include "led.h"
#include "src/SimpleButton/Buttons/ButtonPullup.h"

// Run-Time Variables //
Names names;
SSIDs ssids;
Accesspoints accesspoints;
Stations     stations;
Scan   scan;
Attack attack;
CLI    cli;
#if USE_DISPLAY
DisplayUI displayUI;
#endif

simplebutton::Button* resetButton;

#include "wifi.h"

uint32_t autosaveTime = 0;
uint32_t currentTime  = 0;

bool booted = false;

// Auto AP + station scan once after boot (list ready before user opens web UI)
static bool bootAutoScanStarted = false;
static bool bootAutoScanDone    = false;

void setup() {
    // for random generator
    randomSeed(os_random());

    // start serial
    Serial.begin(115200);
    Serial.println();

    // start SPIFFS
    prnt(SETUP_MOUNT_SPIFFS);
    // bool spiffsError = !LittleFS.begin();
    LittleFS.begin();
    prntln(/*spiffsError ? SETUP_ERROR : */ SETUP_OK);

    // Start EEPROM
    EEPROMHelper::begin(EEPROM_SIZE);

#ifdef FORMAT_SPIFFS
    prnt(SETUP_FORMAT_SPIFFS);
    LittleFS.format();
    prntln(SETUP_OK);
#endif // ifdef FORMAT_SPIFFS

#ifdef FORMAT_EEPROM
    prnt(SETUP_FORMAT_EEPROM);
    EEPROMHelper::format(EEPROM_SIZE);
    prntln(SETUP_OK);
#endif // ifdef FORMAT_EEPROM

    // Format SPIFFS when in boot-loop
    if (/*spiffsError || */ !EEPROMHelper::checkBootNum(BOOT_COUNTER_ADDR)) {
        prnt(SETUP_FORMAT_SPIFFS);
        LittleFS.format();
        prntln(SETUP_OK);

        prnt(SETUP_FORMAT_EEPROM);
        EEPROMHelper::format(EEPROM_SIZE);
        prntln(SETUP_OK);

        EEPROMHelper::resetBootNum(BOOT_COUNTER_ADDR);
    }

    // get time
    currentTime = millis();

    // load settings
    #ifndef RESET_SETTINGS
    settings::load();
    #else // ifndef RESET_SETTINGS
    settings::reset();
    settings::save();
    #endif // ifndef RESET_SETTINGS

    wifi::begin();
    wifi::installPromiscCallback();

#if USE_DISPLAY
    // start display
    if (settings::getDisplaySettings().enabled) {
        displayUI.setup();
        displayUI.mode = DISPLAY_MODE::INTRO;
    }
#endif

    // load everything else
    names.load();
    ssids.load();
    cli.load();

    // create scan.json
    scan.setup();
    clientTracker.begin();
    diag::begin();

    if (settings::getWebSettings().enabled) {
        wifi::startAP(false);
    }

    if (settings::getAllSettings().workmode == 1) {
        bootAutoScanDone = true;
    }

    // dis/enable serial command interface
    if (settings::getCLISettings().enabled) {
        cli.enable();
    } else {
        prntln(SETUP_SERIAL_WARNING);
        Serial.flush();
        Serial.end();
    }

    // STARTED
    prntln(SETUP_STARTED);

    // version
    prntln(DEAUTHER_VERSION);

    // setup LED
    led::setup();

    // setup reset button
    resetButton = new simplebutton::ButtonPullup(RESET_BUTTON);
}

void loop() {
    currentTime = millis();
    diag::loopBegin();

    // P0 + P1: HTTP/DNS + promisc ring drain (bounded)
    wifi::update();

    // P2: registry tick → incremental scan.json
    wifi::tickClients();

    led::update();
    attack.update();
#if USE_DISPLAY
    displayUI.update();
#endif
    cli.update();

    // P3/P4: manual scanNetworks + isolated sniff/handshake sessions
    scan.update();

    DIAG_PHASE(DIAG_P4, "HS", "update");
    hsCaptureUpdate();

    diag::tick();
    ssids.update();  // run random mode, if enabled

    // auto-save
    if (settings::getAutosaveSettings().enabled
        && (currentTime - autosaveTime > settings::getAutosaveSettings().time)) {
        autosaveTime = currentTime;
        names.save(false);
        ssids.save(false);
        settings::save(false);
    }

    if (!booted) {
        booted = true;
        EEPROMHelper::resetBootNum(BOOT_COUNTER_ADDR);
#if USE_DISPLAY && defined(HIGHLIGHT_LED)
        displayUI.setupLED();
#endif
    }

    // Boot: scan APs then clients (~20s sniff, channel hop) while user has not connected yet
    // Skip in repeater workmode — turns softAP off and conflicts with WiFi.scanNetworks
    if (booted && settings::getWebSettings().enabled && !bootAutoScanDone
        && settings::getAllSettings().workmode != 1) {
        if (!bootAutoScanStarted) {
            bootAutoScanStarted = true;
            prntln(F("[Boot] AP + station scan..."));
            scan.start(SCAN_MODE_ALL, 20000, SCAN_MODE_OFF, 0, true, settings::getWifiSettings().channel);
        } else if (!scan.isScanning()) {
            bootAutoScanDone = true;
            clientTracker.exportToStations();
            scan.save(true);
            prntln(F("[Boot] Scan complete, results in /scan.json"));
        }
    }

    resetButton->update();
    if (resetButton->holding(5000)) {
        led::setMode(LED_MODE::SCAN);
#if USE_DISPLAY
        DISPLAY_MODE _mode = displayUI.mode;
        displayUI.mode = DISPLAY_MODE::RESETTING;
        displayUI.update(true);
#endif

        settings::reset();
        settings::save(true);

        delay(2000);

        led::setMode(LED_MODE::IDLE);
#if USE_DISPLAY
        displayUI.mode = _mode;
#endif
    }
}
