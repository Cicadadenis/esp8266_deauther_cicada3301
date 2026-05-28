/* OTA firmware upload via web UI */

#include "firmware_update.h"

#include <Updater.h>
#include <ESP8266WiFi.h>

#include "Attack.h"
#include "Scan.h"
#include "debug.h"

extern "C" {
    #include "user_interface.h"
}

extern Attack attack;
extern Scan   scan;

namespace {

    ESP8266WebServer* g_httpServer = nullptr;
    FirmwareAuthFn    g_authCheck  = nullptr;
    bool              g_otaAuthOk  = false;
    bool              g_otaStarted = false;
    bool              g_otaValidationOk = true;

    // OTA binary validation: require a long magic marker in the uploaded firmware.
    // Short markers (like "Cicada3301") can appear by chance in random binaries.
    static const char OTA_MAGIC[] PROGMEM =
        "CICADA3301__ESP8266_DEAUTHER__FIRMWARE_VALIDATION_MARKER__V1";
    static const uint8_t OTA_MAGIC_LEN = 61;
    uint8_t g_magicPos = 0;

    static void scanForMagic(const uint8_t* buf, size_t len) {
        if (!buf || len == 0) return;
        if (g_magicPos >= OTA_MAGIC_LEN) return; // already found

        for (size_t i = 0; i < len; i++) {
            const char want = (char)pgm_read_byte(&OTA_MAGIC[g_magicPos]);
            if ((char)buf[i] == want) {
                g_magicPos++;
                if (g_magicPos >= OTA_MAGIC_LEN) return;
            } else {
                // restart match; handle immediate prefix overlap
                g_magicPos = ((char)buf[i] == (char)pgm_read_byte(&OTA_MAGIC[0])) ? 1 : 0;
            }
        }
    }

    void prepareForOta() {
        attack.stop();
        if (scan.isSniffing()) scan.stop();
        wifi_promiscuous_enable(0);
        yield();
        WiFi.mode(WIFI_AP);
        yield();
    }

    void handleFirmwareUpdatePost() {
        if (!g_httpServer) return;

        if (!g_otaAuthOk) {
            g_httpServer->send(401, F("text/plain"), F("Unauthorized"));
            return;
        }

        if (!g_otaValidationOk) {
            g_httpServer->send(400, F("text/plain"), F("VALIDATION_FAIL"));
            g_otaStarted = false;
            return;
        }

        if (Update.hasError()) {
            prntln(F("[OTA] failed"));
            Update.printError(Serial);
            g_httpServer->send(500, F("text/plain"), F("FAIL"));
            g_otaStarted = false;
            return;
        }

        g_httpServer->send(200, F("text/plain"), F("OK"));
        delay(200);
        ESP.restart();
    }

    void handleFirmwareUpdateUpload() {
        if (!g_httpServer) return;

        HTTPUpload& upload = g_httpServer->upload();

        if (upload.status == UPLOAD_FILE_START) {
            g_otaAuthOk = g_authCheck && g_authCheck();
            g_otaValidationOk = true;
            g_magicPos = 0;

            if (!g_otaAuthOk) {
                prntln(F("[OTA] rejected: not authorized"));
                return;
            }

            prepareForOta();

            g_otaStarted = false;

            prnt(F("[OTA] file: "));
            prntln(upload.filename);

            uint32_t maxSketch = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
            uint32_t size      = upload.totalSize;

            if (size > 0 && size < maxSketch) {
                if (!Update.begin(size, U_FLASH)) {
                    prntln(F("[OTA] Update.begin(size) failed"));
                    Update.printError(Serial);
                    return;
                }
            } else if (!Update.begin(maxSketch, U_FLASH)) {
                prntln(F("[OTA] Update.begin(max) failed"));
                Update.printError(Serial);
                return;
            }

            g_otaStarted = true;
            prntln(F("[OTA] writing flash..."));
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (!g_otaAuthOk || !g_otaStarted) return;

            scanForMagic(upload.buf, upload.currentSize);

            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                prntln(F("[OTA] write error"));
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (!g_otaAuthOk || !g_otaStarted) return;

            if (g_magicPos < OTA_MAGIC_LEN) {
                prntln(F("[OTA] rejected: validation marker missing"));
                g_otaValidationOk = false;
                // Some ESP8266 core versions don't have Update.abort()
                // Update.end() without commit stops the update session.
                Update.end(false);
                g_otaStarted = false;
                return;
            }

            if (Update.end(true)) {
                prntln(F("[OTA] success"));
            } else {
                prntln(F("[OTA] Update.end failed"));
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
            prntln(F("[OTA] aborted"));
            Update.end();
            g_otaStarted = false;
        }
    }

} // namespace

void firmwareUpdateRegister(ESP8266WebServer& server, FirmwareAuthFn authCheck) {
    g_httpServer = &server;
    g_authCheck  = authCheck;

    server.on(
        "/update",
        HTTP_POST,
        handleFirmwareUpdatePost,
        handleFirmwareUpdateUpload);
}
