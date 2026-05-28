/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

#include "client_tracker.h"

#include "logger.h"
#include "Accesspoints.h"
#include "Scan.h"
#include "Stations.h"
#include "HandshakeCapture.h"

extern Accesspoints accesspoints;
extern Stations     stations;
extern Scan         scan;
extern uint8_t      wifi_channel;
extern uint32_t     currentTime;

extern bool macValid(uint8_t* mac);
extern bool macBroadcast(uint8_t* mac);
extern bool macMulticast(uint8_t* mac);

ClientTracker clientTracker;

ClientTracker::ClientTracker() {
    clear();
    lastTickMs        = 0;
    lastSaveMs        = 0;
    lastMetricsMs     = 0;
    lastPurgeRemoved  = 0;
    registryDirty     = false;
}

void ClientTracker::clear() {
    for (int i = 0; i < CLIENT_TRACKER_MAX; i++) entries[i].used = false;
    registryDirty = false;
}

void ClientTracker::begin() {
    clear();
    lastTickMs       = 0;
    lastSaveMs       = 0;
    lastMetricsMs    = 0;
    lastPurgeRemoved = 0;
    registryDirty    = false;
}

static int8_t pktRssi(uint8_t* buf, uint16_t len) {
    if (len < 14) return 0;
    return (int8_t)buf[0];
}

int ClientTracker::findEntry(const uint8_t* mac) const {
    for (int i = 0; i < CLIENT_TRACKER_MAX; i++) {
        if (entries[i].used && memcmp(entries[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

int ClientTracker::findAccesspoint(const uint8_t* apMac) {
    int c        = accesspoints.count();
    int selected = accesspoints.selected();

    for (int i = 0; i < c; i++) {
        if (memcmp(accesspoints.getMac(i), apMac, 6) != 0) continue;

        if (selected == 1) {
            if (accesspoints.getSelected(i)) return i;
        } else {
            return i;
        }
    }

    return -1;
}

int ClientTracker::findOrAlloc(const uint8_t* mac) {
    int existing = findEntry(mac);

    if (existing >= 0) return existing;

    int      oldest   = -1;
    uint32_t oldestTs = 0xFFFFFFFF;

    for (int i = 0; i < CLIENT_TRACKER_MAX; i++) {
        if (!entries[i].used) {
            memset(&entries[i], 0, sizeof(Entry));
            memcpy(entries[i].mac, mac, 6);
            entries[i].used     = true;
            entries[i].lastSeen = currentTime;
            entries[i].ch       = wifi_channel;
            entries[i].status   = CLIENT_STATUS_ACTIVE;
            entries[i].dirty    = true;
            registryDirty       = true;
            return i;
        }

        if (entries[i].lastSeen < oldestTs) {
            oldestTs = entries[i].lastSeen;
            oldest   = i;
        }
    }

    if (oldest >= 0) {
        memset(&entries[oldest], 0, sizeof(Entry));
        memcpy(entries[oldest].mac, mac, 6);
        entries[oldest].used     = true;
        entries[oldest].lastSeen = currentTime;
        entries[oldest].ch       = wifi_channel;
        entries[oldest].status   = CLIENT_STATUS_ACTIVE;
        entries[oldest].dirty    = true;
        registryDirty            = true;
        return oldest;
    }

    return -1;
}

void ClientTracker::updateClient(const uint8_t* mac, const uint8_t* apMac, int8_t rssi, ClientDetSource type) {
    if (!mac || !macValid((uint8_t*)mac) || macBroadcast((uint8_t*)mac) || macMulticast((uint8_t*)mac)) return;

    int idx = findOrAlloc(mac);

    if (idx < 0) return;

    touchEntry(idx, rssi, type);

    if (apMac && macValid((uint8_t*)apMac) && !macBroadcast((uint8_t*)apMac)) {
        memcpy(entries[idx].apMac, apMac, 6);
    }
}

void ClientTracker::touchEntry(int idx, int8_t rssi, ClientDetSource type) {
    if (idx < 0) return;

    entries[idx].pkts++;
    entries[idx].lastSeen = currentTime;
    entries[idx].ch       = wifi_channel;
    entries[idx].source   = type;
    entries[idx].status   = CLIENT_STATUS_ACTIVE;
    entries[idx].dirty    = true;
    registryDirty         = true;

    if (type == CLIENT_SRC_PROBE) entries[idx].probes++;

    if (rssi != 0) {
        entries[idx].rssi = rssi;

        if (entries[idx].pkts == 1 || rssi < entries[idx].rssiMin) entries[idx].rssiMin = rssi;

        if (entries[idx].pkts == 1 || rssi > entries[idx].rssiMax) entries[idx].rssiMax = rssi;

        // EMA smoothing (no heap): new = (3*old + sample) / 4
        if (entries[idx].pkts == 1) entries[idx].rssiEma = rssi;
        else entries[idx].rssiEma = (int16_t)((entries[idx].rssiEma * 3 + rssi) >> 2);

        entries[idx].rssi = (int8_t)entries[idx].rssiEma;
    }
}

void ClientTracker::tryPair(const uint8_t* a, const uint8_t* b, int8_t rssi, ClientDetSource type) {
    if (!macValid((uint8_t*)a) || macBroadcast((uint8_t*)a) || macMulticast((uint8_t*)a)) return;
    if (!macValid((uint8_t*)b) || macBroadcast((uint8_t*)b) || macMulticast((uint8_t*)b)) return;

    if (findAccesspoint(a) >= 0) {
        updateClient(b, a, rssi, type);
        return;
    }

    if (findAccesspoint(b) >= 0) updateClient(a, b, rssi, type);
}

void ClientTracker::fromMgmt(uint8_t* buf, uint16_t len, int8_t rssi) {
    if (len < 24) return;

    uint8_t fc = buf[12];

    if ((fc == 0xc0) || (fc == 0xa0)) {
        tryPair(&buf[4], &buf[10], rssi, CLIENT_SRC_MGMT);
        if (memcmp(&buf[10], &buf[16], 6) != 0) tryPair(&buf[4], &buf[16], rssi, CLIENT_SRC_MGMT);
        return;
    }

    if ((fc == 0x00) || (fc == 0x20)) {
        tryPair(&buf[10], &buf[16], rssi, CLIENT_SRC_MGMT);
        tryPair(&buf[10], &buf[4], rssi, CLIENT_SRC_MGMT);
        return;
    }

    if (fc == 0x40) {
        updateClient(&buf[10], nullptr, rssi, CLIENT_SRC_PROBE);
        tryPair(&buf[4], &buf[10], rssi, CLIENT_SRC_PROBE);
        tryPair(&buf[16], &buf[10], rssi, CLIENT_SRC_PROBE);
        return;
    }

    if (fc == 0x80) {
        if (len >= 34) tryPair(&buf[22], &buf[28], rssi, CLIENT_SRC_BEACON);
    }
}

void ClientTracker::onFrame(uint8_t* buf, uint16_t len) {
    if (!buf || len < 24) return;

    int8_t rssi = pktRssi(buf, len);
    uint8_t fc  = buf[12];

    if ((fc == 0xc0) || (fc == 0xa0) || (fc == 0x00) || (fc == 0x20) || (fc == 0x40) || (fc == 0x80)) {
        fromMgmt(buf, len, rssi);
        return;
    }

    if (fc == 0x50) return;

    if (len < 28) return;

    uint8_t* macTo   = &buf[16];
    uint8_t* macFrom = &buf[22];

    if (macBroadcast(macTo) || macBroadcast(macFrom) || !macValid(macTo) || !macValid(macFrom) || macMulticast(macTo) ||
        macMulticast(macFrom)) return;

    tryPair(&buf[4], &buf[10], rssi, CLIENT_SRC_DATA);

    if (len >= 28) tryPair(macFrom, macTo, rssi, CLIENT_SRC_DATA);
}

bool ClientTracker::registryHasMac(const uint8_t* mac) const {
    return findEntry(mac) >= 0;
}

void ClientTracker::purgeStale() {
    int     removed = 0;
    uint8_t purged  = 0;

    for (int i = 0; i < CLIENT_TRACKER_MAX; i++) {
        if (!entries[i].used) continue;

        if (currentTime - entries[i].lastSeen > CLIENT_ACTIVE_MS) entries[i].status = CLIENT_STATUS_IDLE;

        if (currentTime - entries[i].lastSeen > CLIENT_STALE_MS) {
            entries[i].used  = false;
            entries[i].dirty = false;
            registryDirty    = true;
            purged++;
        }
    }

    lastPurgeRemoved = purged;

    for (int i = stations.count() - 1; i >= 0 && removed < CLIENT_PURGE_MAX; i--) {
        if (!registryHasMac(stations.getMac(i))) {
            stations.remove(i);
            removed++;
            registryDirty = true;
        }
    }
}

void ClientTracker::applySnapshotToStations() {
    if (!registryDirty) return;

    for (int i = 0; i < CLIENT_TRACKER_MAX; i++) {
        if (!entries[i].used || !entries[i].dirty) continue;
        if (entries[i].status == CLIENT_STATUS_IDLE) continue;

        int apIdx = findAccesspoint(entries[i].apMac);

        if (apIdx < 0) continue;

        stations.add(entries[i].mac, accesspoints.getID(apIdx));
        entries[i].dirty = false;
    }

    registryDirty = false;
}

void ClientTracker::exportToStations() {
    for (int i = 0; i < CLIENT_TRACKER_MAX; i++) {
        if (!entries[i].used || entries[i].pkts == 0) continue;

        int apIdx = findAccesspoint(entries[i].apMac);

        if (apIdx < 0) continue;

        stations.add(entries[i].mac, accesspoints.getID(apIdx));
        entries[i].dirty = false;
    }

    stations.changed = true;
    registryDirty    = false;
}

void ClientTracker::tick() {
    if (currentTime - lastTickMs < CLIENT_TICK_MS) return;

    lastTickMs = currentTime;

    purgeStale();

    if (!scan.isSniffing() && !hsCaptureActive()) applySnapshotToStations();

    if (registryDirty && (currentTime - lastSaveMs > CLIENT_SAVE_MS)) {
        lastSaveMs = currentTime;

        if (stations.changed) scan.save(false);
    }

    if (currentTime - lastMetricsMs >= DIAG_TRACKER_METRICS_MS) {
        lastMetricsMs = currentTime;

        uint32_t total = 0, active = 0, idle = 0, purged = 0;
        int16_t  rssiAvg = 0;

        getMetrics(&total, &active, &idle, &rssiAvg, &purged);

        DIAG_LOGF(DIAG_INFO, "TRACKER",
                  "clients=%lu active=%lu idle=%lu rssi_avg=%d stale_purged=%lu",
                  (unsigned long)total, (unsigned long)active, (unsigned long)idle, (int)rssiAvg,
                  (unsigned long)purged);
    }
}

void ClientTracker::getMetrics(uint32_t* total, uint32_t* active, uint32_t* idle, int16_t* rssiAvg,
                               uint32_t* purgeRemoved) const {
    uint32_t n = 0, act = 0, idl = 0;
    int32_t  rssiSum = 0;
    uint32_t rssiCnt = 0;

    for (int i = 0; i < CLIENT_TRACKER_MAX; i++) {
        if (!entries[i].used) continue;

        n++;

        if (entries[i].status == CLIENT_STATUS_ACTIVE) act++;
        else idl++;

        if (entries[i].rssiEma != 0) {
            rssiSum += entries[i].rssiEma;
            rssiCnt++;
        }
    }

    if (total) *total = n;

    if (active) *active = act;

    if (idle) *idle = idl;

    if (rssiAvg) *rssiAvg = rssiCnt ? (int16_t)(rssiSum / (int32_t)rssiCnt) : 0;

    if (purgeRemoved) *purgeRemoved = lastPurgeRemoved;
}

uint32_t ClientTracker::count() const {
    uint32_t n = 0;

    for (int i = 0; i < CLIENT_TRACKER_MAX; i++)
        if (entries[i].used) n++;

    return n;
}
