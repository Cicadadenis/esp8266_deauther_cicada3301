/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

#include "Scan.h"

#include "settings.h"
#include "wifi.h"
#include "client_tracker.h"
#include "Attack.h"
#include "HandshakeCapture.h"
#include "logger.h"

extern Attack attack;

namespace {
uint32_t diag_scan_start_us = 0;
uint8_t  diag_scan_last_mode = SCAN_MODE_OFF;

static int8_t promiscRssi(uint8_t* buf, uint16_t len) {
    if (len < 14) return 0;
    return (int8_t)buf[0];
}

static bool isApMgmtBeacon(uint8_t fc) {
    return fc == 0x80 || fc == 0x50; // beacon / probe response
}

// ESP8266 promisc: 12-byte RX meta, 802.11 @12 — addr1/2/3 @16/22/28
static uint8_t* findApBssid(uint8_t* buf, uint16_t len) {
    static const uint8_t offs[] = {22, 28, 16, 10, 4};

    for (unsigned i = 0; i < sizeof(offs); i++) {
        if (offs[i] + 6 > len) continue;

        uint8_t* m = &buf[offs[i]];

        if (macValid(m) && !macBroadcast(m) && !macMulticast(m)) return m;
    }

    return nullptr;
}

static void parseApInfoElements(uint8_t* buf, uint16_t len, uint16_t pos, char* ssid, uint8_t& ch,
                                uint8_t& enc, bool& hidden, bool& hasDsChannel) {
    while (pos + 2 < len) {
        uint8_t id   = buf[pos];
        uint8_t elen = buf[pos + 1];

        if (elen == 0 || pos + 2 + elen > len) break;

        if (id == 0) {
            hidden = (elen == 0);

            if (elen > 0 && elen < 32) {
                memcpy(ssid, &buf[pos + 2], elen);
                ssid[elen] = '\0';
            }
        } else if (id == 3 && elen >= 1) {
            ch            = buf[pos + 2];
            hasDsChannel  = true;
        } else if (id == 48) {
            enc = ENC_TYPE_CCMP;
        } else if (id == 221 && elen >= 4) {
            if (enc == ENC_TYPE_NONE || enc == ENC_TYPE_WEP) enc = ENC_TYPE_TKIP;
        }

        pos += 2 + elen;
    }
}
} // namespace

Scan::Scan() {
    list = new SimpleList<uint16_t>;
}

void Scan::tryAddStation(uint8_t* staMac, uint8_t* apMac) {
    if (!macValid(staMac) || macBroadcast(staMac) || macMulticast(staMac)) return;

    int accesspointNum = findAccesspoint(apMac);

    if (accesspointNum >= 0) stations.add(staMac, accesspoints.getID(accesspointNum));
}

void Scan::tryAddStationPair(uint8_t* a, uint8_t* b) {
    if (!macValid(a) || !macValid(b)) return;

    int accesspointNum = findAccesspoint(a);

    if (accesspointNum >= 0) {
        tryAddStation(b, a);
        return;
    }

    accesspointNum = findAccesspoint(b);

    if (accesspointNum >= 0) tryAddStation(a, b);
}

void Scan::snifferFromMgmt(uint8_t* buf, uint16_t len) {
    if (len < 24) return;

    uint8_t fc = buf[12];

    // deauth / disassoc — часто единственные кадры у «тихих» клиентов
    if ((fc == 0xc0) || (fc == 0xa0)) {
        tmpDeauths++;
        tryAddStationPair(&buf[4], &buf[10]);

        if (memcmp(&buf[10], &buf[16], 6) != 0) tryAddStationPair(&buf[4], &buf[16]);

        return;
    }

    // association / reassociation request
    if ((fc == 0x00) || (fc == 0x20)) {
        tryAddStation(&buf[10], &buf[16]);
        tryAddStation(&buf[10], &buf[4]);
        return;
    }

    // probe request — клиент ищет сеть (addr2 = STA)
    if (fc == 0x40) {
        tryAddStationPair(&buf[4], &buf[10]);
        tryAddStationPair(&buf[16], &buf[10]);
    }
}

void Scan::sniffer(uint8_t* buf, uint16_t len) {
    if (!isSniffing()) return;

    packets++;
    sniffTotal++;

    if (hsCaptureActive()) {
        hsCaptureOnFrame(buf, len);
        return;
    }

    if (len < 24) return;

    uint8_t fc = buf[12];

    if ((fc == 0xc0) || (fc == 0xa0) || (fc == 0x00) || (fc == 0x20) || (fc == 0x40)) {
        snifferFromMgmt(buf, len);
        return;
    }

    // drop beacons and probe responses
    if ((fc == 0x80) || (fc == 0x50)) return;

    if (len < 28) return;

    uint8_t* macTo   = &buf[16];
    uint8_t* macFrom = &buf[22];

    if (macBroadcast(macTo) || macBroadcast(macFrom) || !macValid(macTo) || !macValid(macFrom) || macMulticast(macTo) ||
        macMulticast(macFrom)) return;

    tryAddStationPair(&buf[4], &buf[10]);

    if (len >= 28) tryAddStationPair(macFrom, macTo);
}

int Scan::findAccesspoint(uint8_t* mac) {
    int c        = accesspoints.count();
    int selected = accesspoints.selected();

    for (int i = 0; i < c; i++) {
        if (memcmp(accesspoints.getMac(i), mac, 6) != 0) continue;

        if (selected == 1) {
            if (accesspoints.getSelected(i)) return i;
        } else {
            return i;
        }
    }

    return -1;
}

void Scan::refreshApsForStScan(uint8_t channel, bool channelHop) {
    // Never block HTTP/AP path with synchronous scanNetworks
    if (wifi::isApActive()) return;

    uint8_t selMac[6];
    bool    hadSel = false;

    for (int i = 0; i < accesspoints.count(); i++) {
        if (accesspoints.getSelected(i)) {
            memcpy(selMac, accesspoints.getMac(i), 6);
            hadSel = true;
            break;
        }
    }

    accesspoints.removeAll();
    stations.removeAll();
    WiFi.scanDelete();
    yield();

    prntln("ST: rescan APs...");
    // Blocking only when softAP is down (rare); never called while AP always-on is active
    int16_t n = WiFi.scanNetworks(false, true);

    if (n < 0) n = 0;
    if (n > 64) n = 64;

    for (int16_t i = 0; i < n; i++) {
        if (channelHop || (WiFi.channel(i) == channel)) accesspoints.add(i, false);
    }

    accesspoints.sort();

    if (hadSel) {
        for (int i = 0; i < accesspoints.count(); i++) {
            if (memcmp(accesspoints.getMac(i), selMac, 6) == 0) {
                accesspoints.select(i);
                break;
            }
        }
    }

    prnt("ST: APs on scan list=");
    prntln(String(accesspoints.count()));
}

void Scan::start(uint8_t mode) {
    start(mode, sniffTime, scan_continue_mode, continueTime, channelHop, wifi_channel);
}

void Scan::start(uint8_t mode, uint32_t time, uint8_t nextmode, uint32_t continueTime, bool channelHop,
                 uint8_t channel) {
    if (mode != SCAN_MODE_OFF) stop();

    if (mode != SCAN_MODE_OFF) {
        diag_scan_start_us  = micros();
        diag_scan_last_mode = mode;
        DIAG_LOGF(DIAG_INFO, "SCAN", "start mode=%u ch=%u time_ms=%lu hop=%u",
                  (unsigned)mode, (unsigned)channel, (unsigned long)time, (unsigned)(channelHop ? 1 : 0));
    }

    setWifiChannel(channel, true);
    Scan::continueStartTime  = currentTime;
    Scan::snifferPacketTime  = continueStartTime;
    Scan::snifferOutputTime  = continueStartTime;
    Scan::continueTime       = continueTime;
    Scan::sniffTime          = time;
    Scan::channelHop         = channelHop;
    Scan::scanMode           = mode;
    Scan::scan_continue_mode = nextmode;

    if ((sniffTime > 0) && (sniffTime < 1000)) sniffTime = 1000;

    // Serial.printf("mode: %u, time: %u, continue-mode: %u, continueTime: %u, channelHop: %u, channel: %u\r\n", mode,
    // time, scan_continue_mode, continueTime, channelHop, channel);

    /* AP Scan */
    if ((mode == SCAN_MODE_APS) || (mode == SCAN_MODE_ALL)) {
        accesspoints.removeAll();
        stations.removeAll();
        scan_networks_active = true;
        prntln(SC_START_AP);

        // softAP + promiscuous + scanNetworks together → WDT reboot.
        // softAP + promiscuous alone → ring stays 0 (no beacons). Use scanNetworks with promisc off.
        if (wifi::isApActive()) {
            passive_ap_scan = false;
            wifi::suspendMonitorMode();
            yield();
            WiFi.scanDelete();
            yield();
            WiFi.scanNetworks(true, true);
            DIAG_LOG(DIAG_INFO, "SCAN", "AP scan scanNetworks (promisc suspended, softAP on)");
        } else {
            passive_ap_scan = false;
            WiFi.scanDelete();
            yield();
            WiFi.scanNetworks(true, true);
        }
    }

    /* Station Scan */
    else if (mode == SCAN_MODE_STATIONS) {
        if (attack.isRunning()) attack.stop();

        // если выбрана одна AP — фиксируем её канал
        if (!channelHop && accesspoints.selected() == 1) {
            for (int i = 0; i < accesspoints.count(); i++) {
                if (accesspoints.getSelected(i)) {
                    channel = accesspoints.getCh(i);
                    break;
                }
            }
        }

        // Station / handshake sniff needs ST mode (softAP off) — same as upstream deauther
        if (!wifi::isApActive() || accesspoints.count() < 1) {
            if (!hsCaptureActive()) refreshApsForStScan(channel, channelHop);
        }
        apStoppedForScan = false;
        sniffKeepsAp     = false;

        if (accesspoints.count() < 1) {
            prntln(SC_ERROR_NO_AP);
            start(SCAN_MODE_OFF);
            return;
        }

        setWifiChannel(channel, true);
        stations.removeAll();
        sniffTotal      = 0;
        stationKickTime = currentTime;

        snifferStartTime = currentTime;
        prnt(SC_START_CLIENT);

        if (sniffTime > 0) prnt(String(sniffTime / 1000) + S);
        else prnt(SC_INFINITELY);

        if (!channelHop) {
            prnt(SC_ON_CHANNEL);
            prnt(wifi_channel);
        }
        prntln();

        yield();
        setWifiChannel(channel, true);

        wifi::pauseApForSniff(channel);
    }

    else if (mode == SCAN_MODE_SNIFFER) {
        deauths          = tmpDeauths;
        tmpDeauths       = 0;
        snifferStartTime = currentTime;
        prnt(SS_START_SNIFFER);

        if (sniffTime > 0) prnt(String(sniffTime / 1000) + S);
        else prnt(SC_INFINITELY);
        prnt(SC_ON_CHANNEL);
        prntln(channelHop ? str(SC_ONE_TO) + (String)14 : (String)wifi_channel);

        apStoppedForScan = false;
        wifi::pauseApForSniff(channel);
    }

    /* Stop scan */
    else if (mode == SCAN_MODE_OFF) {
        if (passive_ap_scan) passive_ap_scan = false;

        wifi::restoreApAfterSniff();

        if (diag_scan_last_mode != SCAN_MODE_OFF) {
            uint32_t dur = micros() - diag_scan_start_us;
            DIAG_LOGF(DIAG_INFO, "SCAN", "complete prev_mode=%u duration_us=%lu",
                      (unsigned)diag_scan_last_mode, (unsigned long)dur);
            diag_scan_last_mode = SCAN_MODE_OFF;
        }

        scan_networks_active = false;

        if (sniffKeepsAp) {
            ::setWifiChannel(settings::getWifiSettings().channel, true);
        }

        sniffKeepsAp     = false;
        apStoppedForScan = false;

        if (wifi::isApActive() && !wifi::isRepeaterWorkmode()) wifi::enableMonitorMode();
        else wifi_promiscuous_enable(false);

        yield();

        prntln(SC_STOPPED);
        save(true);

        if (scan_continue_mode != SCAN_MODE_OFF) {
            prnt(SC_RESTART);
            prnt(int(continueTime / 1000));
            prntln(SC_CONTINUE);
        }
    }

    /* ERROR */
    else {
        prnt(SC_ERROR_MODE);
        prntln(mode);
        return;
    }
}

void Scan::update() {
    DIAG_PHASE(DIAG_P3, "SCAN", "update");

    if (scanMode == SCAN_MODE_OFF) {
        // Manual-only: no auto-restart while AP is up (scanNetworks decoupled from loop)
        if (scan_continue_mode != SCAN_MODE_OFF && !wifi::isApActive()) {
            if (currentTime - continueStartTime > continueTime) start(scan_continue_mode);
        }
        return;
    }

    // sniffer
    if (isSniffing()) {
        // update packet list every 1s
        if (currentTime - snifferPacketTime > 1000) {
            snifferPacketTime = currentTime;
            list->add(packets);

            if (list->size() > SCAN_PACKET_LIST_SIZE) list->remove(0);
            deauths    = tmpDeauths;
            tmpDeauths = 0;
            packets    = 0;
        }

        // print status every 3s
        if (currentTime - snifferOutputTime > 3000) {
            char s[100];

            static uint32_t hsPrevRx = 0;
            uint32_t statusPkts      = packets;
            uint32_t statusStations  = stations.count();

            if (hsCaptureActive()) {
                uint32_t hsNow = hsGetFramesSeen();
                statusPkts     = hsNow >= hsPrevRx ? (hsNow - hsPrevRx) : hsNow;
                hsPrevRx       = hsNow;
                statusStations = clientTracker.count();
            } else {
                hsPrevRx = 0;
            }

            if (sniffTime > 0) {
                sprintf(s, str(SC_OUTPUT_A).c_str(), getPercentage(), statusPkts, statusStations, deauths);
            } else {
                sprintf(s, str(SC_OUTPUT_B).c_str(), statusPkts, statusStations, deauths);
            }
            prnt(String(s));
            DIAG_LOGF(DIAG_INFO, "SCAN",
                      "sniff pkts=%u st=%u isr=%lu hs_mask=%u hs_rx=%lu hs_data=%lu hs_eapol=%lu",
                      (unsigned)statusPkts, (unsigned)statusStations, (unsigned long)wifi::promiscIsrCount(),
                      (unsigned)(hsCaptureActive() ? hsGetMsgMask() : 0),
                      (unsigned long)(hsCaptureActive() ? hsGetFramesSeen() : sniffTotal),
                      (unsigned long)(hsCaptureActive() ? hsGetFramesData() : 0),
                      (unsigned long)(hsCaptureActive() ? hsGetEapolFound() : 0));
            snifferOutputTime = currentTime;
        }

        // channel hopping
        if (channelHop && (currentTime - snifferChannelTime > settings::getSnifferSettings().channel_time)) {
            snifferChannelTime = currentTime;

            if (scanMode == SCAN_MODE_STATIONS) nextChannel();  // go to next channel an AP is on
            else setChannel(wifi_channel + 1);                  // go to next channel
        }

        // deauth kick — провоцирует трафик клиентов (только лабораторная AP)
        if (scanMode == SCAN_MODE_STATIONS && !hsCaptureActive() &&
            (currentTime - stationKickTime > 6000)) {
            stationKickTime = currentTime;

            for (int i = 0; i < accesspoints.count(); i++) {
                if (accesspoints.getSelected(i)) attack.deauthAP(i);
            }
        }
    }

    // Passive AP scan (promiscuous beacons, softAP stays on)
    if (passive_ap_scan && ((scanMode == SCAN_MODE_APS) || (scanMode == SCAN_MODE_ALL))) {
        if (channelHop && (currentTime - passive_ap_hop_ms >= PASSIVE_AP_HOP_MS)) {
            passive_ap_hop_ms = currentTime;
            setChannel(wifi_channel >= 14 ? 1 : wifi_channel + 1);
        }

        if (currentTime - snifferOutputTime > 3000) {
            snifferOutputTime = currentTime;
            prnt(F("AP scan: "));
            prnt(String(accesspoints.count()));
            prntln(F(" networks"));
        }

        if (accesspoints.changed && (currentTime - passive_ap_save_ms >= 2500)) {
            passive_ap_save_ms = currentTime;
            save(false);
        }

        if (sniffTime > 0 && (currentTime - snifferStartTime >= sniffTime)) {
            finishPassiveApScan();
        }

        return;
    }

    // APs — non-blocking poll (one batch per update() call)
    if ((scanMode == SCAN_MODE_APS) || (scanMode == SCAN_MODE_ALL)) {
        int16_t results = WiFi.scanComplete();

        if (results == -1) return; // async scan still running

        if (results < 0) {
            DIAG_LOGF(DIAG_WARN, "SCAN", "scanNetworks error=%d timeout_us=%lu", (int)results,
                      (unsigned long)(micros() - diag_scan_start_us));
            scan_networks_active = false;
            WiFi.scanDelete();

            if (wifi::isApActive() && !wifi::isRepeaterWorkmode()) wifi::enableMonitorMode();

            start(SCAN_MODE_OFF);
            return;
        }

        if (results >= 0) {
            DIAG_LOGF(DIAG_INFO, "SCAN", "scanNetworks complete n=%d duration_us=%lu", (int)results,
                      (unsigned long)(micros() - diag_scan_start_us));
            for (int16_t i = 0; i < results && i < 256; i++) {
                if (channelHop || (WiFi.channel(i) == wifi_channel)) accesspoints.add(i, false);
            }
            accesspoints.sort();
            accesspoints.printAll();
            accesspoints.materializeFromWifiScan();
            WiFi.scanDelete();

            if (wifi::isApActive() && !wifi::isRepeaterWorkmode()) wifi::enableMonitorMode();

            if (scanMode == SCAN_MODE_ALL) {
                yield();
                scan_networks_active = false;
                start(SCAN_MODE_STATIONS);
            } else {
                scan_networks_active = false;
                start(SCAN_MODE_OFF);
            }
        }
    }

    // Stations
    else if ((sniffTime > 0) && (currentTime > snifferStartTime + sniffTime)) {
        if (scanMode == SCAN_MODE_STATIONS || scanMode == SCAN_MODE_ALL) {
            clientTracker.exportToStations();
            stations.sort();
            stations.printAll();
        }
        DIAG_LOGF(DIAG_INFO, "SCAN", "sniff timeout mode=%u duration_us=%lu stations=%d isr=%lu",
                  (unsigned)scanMode, (unsigned long)(micros() - diag_scan_start_us), stations.count(),
                  (unsigned long)wifi::promiscIsrCount());

        if (hsCaptureActive()) hsCaptureOnSniffEnded();

        start(SCAN_MODE_OFF);
    }
}

void Scan::setup() {
    accesspoints.removeAll();
    stations.removeAll();
    sniffTotal = 0;
    save(true);
}

void Scan::stop() {
    scan_continue_mode = SCAN_MODE_OFF;
    start(SCAN_MODE_OFF);
}

void Scan::setChannel(uint8_t ch) {
    if (ch > 14) ch = 1;
    else if (ch < 1) ch = 14;

    if (wifi::isApActive()) wifi::setMonitorChannel(ch);
    else {
        wifi_promiscuous_enable(0);
        setWifiChannel(ch, true);
        wifi_promiscuous_enable(1);
    }
}

void Scan::nextChannel() {
    if (accesspoints.count() > 1) {
        uint8_t ch = wifi_channel;

        do {
            ch++;

            if (ch > 14) ch = 1;
        } while (!apWithChannel(ch));
        setChannel(ch);
    }
}

bool Scan::apWithChannel(uint8_t ch) {
    for (int i = 0; i < accesspoints.count(); i++)
        if (accesspoints.getCh(i) == ch) return true;

    return false;
}

void Scan::save(bool force, String filePath) {
    String tmp = FILE_PATH;

    FILE_PATH = filePath;
    save(true);
    FILE_PATH = tmp;
}

bool Scan::peekJsonCache(const String*& out) const {
    if (!jsonCacheValid || jsonCache.length() == 0) return false;

    if (accesspoints.changed || stations.changed || sniffTotal != sniffTotalAtSave) return false;

    out = &jsonCache;
    return true;
}

bool Scan::getJsonCache(String& out) const {
    const String* cached = nullptr;

    if (!peekJsonCache(cached)) return false;

    out = *cached;
    return true;
}

bool Scan::hasJsonCache() const {
    return jsonCacheValid && jsonCache.length() > 0;
}

uint32_t Scan::getJsonVersion() const {
    return jsonVersion;
}

void Scan::saveScanIfNeeded() {
    save(false);
}

void Scan::save(bool force) {
    if (!force) {
        if (accesspoints.changed || stations.changed || sniffTotal != sniffTotalAtSave) scanDirty = true;

        if (!scanDirty) {
            DIAG_LOGF(DIAG_DEBUG, "SCAN", "save scan skipped (clean)");
            return;
        }
    }

#if DIAG_ENABLE
    const uint32_t saveScanT0 = millis();
#endif

    const size_t prevLen = jsonCache.length();

    jsonCache.remove(0);
    if (prevLen > 64) jsonCache.reserve(prevLen);
    else jsonCache.reserve(512);

    jsonCache += String(OPEN_CURLY_BRACKET) + String(DOUBLEQUOTES) + str(SC_JSON_APS) + String(DOUBLEQUOTES) + String(
        DOUBLEPOINT) + String(OPEN_BRACKET);

    String buf;
    uint32_t apCount = accesspoints.count();

    for (uint32_t i = 0; i < apCount; i++) {
        buf += String(OPEN_BRACKET) + String(DOUBLEQUOTES) + escape(accesspoints.getSSID(i)) + String(DOUBLEQUOTES) +
               String(COMMA);
        buf += String(DOUBLEQUOTES) + escape(accesspoints.getNameStr(i)) + String(DOUBLEQUOTES) + String(COMMA);
        buf += String(accesspoints.getCh(i)) + String(COMMA);
        buf += String(accesspoints.getRSSI(i)) + String(COMMA);
        buf += String(DOUBLEQUOTES) + accesspoints.getEncStr(i) + String(DOUBLEQUOTES) + String(COMMA);
        buf += String(DOUBLEQUOTES) + accesspoints.getMacStr(i) + String(DOUBLEQUOTES) + String(COMMA);
        buf += String(DOUBLEQUOTES) + accesspoints.getVendorStr(i) + String(DOUBLEQUOTES) + String(COMMA);
        buf += b2s(accesspoints.getSelected(i)) + String(CLOSE_BRACKET);

        if (i < apCount - 1) buf += String(COMMA);

        if (buf.length() >= 1024) {
            jsonCache += buf;
            buf = String();
        }
    }

    buf += String(CLOSE_BRACKET) + String(COMMA) + String(DOUBLEQUOTES) + str(SC_JSON_STATIONS) + String(DOUBLEQUOTES) +
           String(DOUBLEPOINT) + String(OPEN_BRACKET);
    uint32_t stationCount = stations.count();

    for (uint32_t i = 0; i < stationCount; i++) {
        buf += String(OPEN_BRACKET) + String(DOUBLEQUOTES) + stations.getMacStr(i) + String(DOUBLEQUOTES) +
               String(COMMA);
        buf += String(stations.getCh(i)) + String(COMMA);
        buf += String(DOUBLEQUOTES) + stations.getNameStr(i) + String(DOUBLEQUOTES) + String(COMMA);
        buf += String(DOUBLEQUOTES) + stations.getVendorStr(i) + String(DOUBLEQUOTES) + String(COMMA);
        buf += String(*stations.getPkts(i)) + String(COMMA);
        buf += String(stations.getAP(i)) + String(COMMA);
        buf += String(DOUBLEQUOTES) + stations.getTimeStr(i) + String(DOUBLEQUOTES) + String(COMMA);
        buf += b2s(stations.getSelected(i)) + String(CLOSE_BRACKET);

        if (i < stationCount - 1) buf += String(COMMA);

        if (buf.length() >= 1024) {
            jsonCache += buf;
            buf = String();
        }
    }

    buf += String(CLOSE_BRACKET) + String(COMMA) + String(DOUBLEQUOTES) + "sniff_pkts" + String(DOUBLEQUOTES) +
           String(DOUBLEPOINT) + String(sniffTotal) + String(CLOSE_CURLY_BRACKET);
    jsonCache += buf;

    if (!writeFile(FILE_PATH, jsonCache)) {
        jsonCacheValid = false;
        scanDirty      = true;
        prnt(F_ERROR_SAVING);
        prntln(FILE_PATH);
        return;
    }

    jsonCacheValid   = true;
    jsonVersion++;
    scanDirty        = false;
    sniffTotalAtSave = sniffTotal;
    accesspoints.changed = false;
    stations.changed     = false;
    prnt(SC_SAVED_IN);
    prntln(FILE_PATH);

#if DIAG_ENABLE
    DIAG_LOGF(DIAG_DEBUG, "SCAN", "save scan %lu ms", (unsigned long)(millis() - saveScanT0));
#endif
}

uint32_t Scan::countSelected() {
    return accesspoints.selected() + stations.selected() + names.selected();
}

uint32_t Scan::countAll() {
    return accesspoints.count() + stations.count() + names.count();
}

bool Scan::isScanning() {
    return scanMode != SCAN_MODE_OFF;
}

bool Scan::isSniffing() {
    return scanMode == SCAN_MODE_STATIONS || scanMode == SCAN_MODE_SNIFFER;
}

bool Scan::isScanNetworksActive() {
    return scan_networks_active && !passive_ap_scan;
}

bool Scan::isPassiveApScan() const {
    return passive_ap_scan;
}

void Scan::finishPassiveApScan() {
    accesspoints.sort();
    accesspoints.printAll();
    save(true);

    passive_ap_scan    = false;
    scan_networks_active = false;

    if (wifi::isApActive()) wifi::setMonitorChannel(settings::getWifiSettings().channel);

    DIAG_LOGF(DIAG_INFO, "SCAN", "passive AP scan done aps=%d duration_ms=%lu", accesspoints.count(),
              (unsigned long)(currentTime - snifferStartTime));

    if (scanMode == SCAN_MODE_ALL) {
        yield();
        start(SCAN_MODE_STATIONS);
    } else {
        start(SCAN_MODE_OFF);
    }
}

void Scan::parseBeacon(uint8_t* buf, uint16_t len, int8_t rssi) {
    if (len < 40) return;

    uint8_t* bssid = findApBssid(buf, len);

    if (!bssid) return;

    char     ssid[33];
    uint8_t  ch  = wifi_channel;
    uint8_t  enc = ENC_TYPE_NONE;
    bool     hidden = false;
    bool     hasDsChannel = false;

    ssid[0] = '\0';

    if (len >= 48) {
        uint16_t cap = ((uint16_t)buf[46] & 0xFF) | (((uint16_t)buf[47] & 0xFF) << 8);

        if (cap & 0x0010) enc = ENC_TYPE_WEP;
    }

    // IEs after 24-byte mgmt hdr + 12-byte beacon/probe fixed fields
    parseApInfoElements(buf, len, 48, ssid, ch, enc, hidden, hasDsChannel);

    if (ssid[0] == '\0' && len >= 52) parseApInfoElements(buf, len, 36, ssid, ch, enc, hidden, hasDsChannel);

    if (!channelHop && hasDsChannel && ch != wifi_channel) return;

    accesspoints.addOrUpdatePassive(bssid, ssid, ch, rssi, enc, hidden);
}

void Scan::onPromiscFrame(uint8_t* buf, uint16_t len) {
    if (!passive_ap_scan || !buf || len < 36) return;

    if (!isApMgmtBeacon(buf[12])) return;

    parseBeacon(buf, len, promiscRssi(buf, len));
}

uint8_t Scan::getPercentage() {
    if (!isSniffing()) return 0;

    return (currentTime - snifferStartTime) / (sniffTime / 100);
}

void Scan::selectAll() {
    accesspoints.selectAll();
    stations.selectAll();
    names.selectAll();
}

void Scan::deselectAll() {
    accesspoints.deselectAll();
    stations.deselectAll();
    names.deselectAll();
}

void Scan::printAll() {
    accesspoints.printAll();
    stations.printAll();
    names.printAll();
    ssids.printAll();
}

void Scan::printSelected() {
    accesspoints.printSelected();
    stations.printSelected();
    names.printSelected();
}

uint32_t Scan::getPackets(int i) {
    if (list->size() < SCAN_PACKET_LIST_SIZE) {
        uint8_t translatedNum = SCAN_PACKET_LIST_SIZE - list->size();

        if (i >= translatedNum) return list->get(i - translatedNum);

        return 0;
    } else {
        return list->get(i);
    }
}

String Scan::getMode() {
    switch (scanMode) {
        case SCAN_MODE_OFF:
            return str(SC_MODE_OFF);

        case SCAN_MODE_APS:
            return str(SC_MODE_AP);

        case SCAN_MODE_STATIONS:
            return str(SC_MODE_ST);

        case SCAN_MODE_ALL:
            return str(SC_MODE_ALL);

        case SCAN_MODE_SNIFFER:
            return str(SC_MODE_SNIFFER);

        default:
            return String();
    }
}

double Scan::getScaleFactor(uint8_t height) {
    return (double)height / (double)getMaxPacket();
}

uint32_t Scan::getMaxPacket() {
    uint16_t max = 0;

    for (uint8_t i = 0; i < list->size(); i++) {
        if (list->get(i) > max) max = list->get(i);
    }
    return max;
}

uint32_t Scan::getPacketRate() {
    return list->get(list->size() - 1);
}