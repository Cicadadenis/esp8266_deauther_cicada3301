/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

#pragma once

#include "Arduino.h"

#define CLIENT_TRACKER_MAX   64
#define CLIENT_STALE_MS      120000
#define CLIENT_ACTIVE_MS     15000
#define CLIENT_TICK_MS       2000
#define CLIENT_SAVE_MS       8000
#define CLIENT_PURGE_MAX     4

enum ClientDetSource : uint8_t {
    CLIENT_SRC_PROBE  = 0,
    CLIENT_SRC_BEACON = 1,
    CLIENT_SRC_DATA   = 2,
    CLIENT_SRC_MGMT   = 3
};

enum ClientStatus : uint8_t {
    CLIENT_STATUS_IDLE   = 0,
    CLIENT_STATUS_ACTIVE = 1
};

// Fixed-size in-memory registry — single source of truth for stations
class ClientTracker {
    public:
        ClientTracker();

        void begin();
        void onFrame(uint8_t* buf, uint16_t len);
        void tick();

        void updateClient(const uint8_t* mac, const uint8_t* apMac, int8_t rssi, ClientDetSource type);

        uint32_t count() const;
        void clear();

        void getMetrics(uint32_t* total, uint32_t* active, uint32_t* idle, int16_t* rssiAvg,
                        uint32_t* purgeRemoved) const;

        // Push registry into stations list (e.g. after station sniff)
        void exportToStations();

    private:
        struct Entry {
            uint8_t          mac[6];
            uint8_t          apMac[6];
            int8_t           rssi;
            int8_t           rssiMin;
            int8_t           rssiMax;
            int16_t          rssiEma;
            uint8_t          ch;
            uint32_t         lastSeen;
            uint32_t         probes;
            uint32_t         pkts;
            ClientDetSource  source;
            ClientStatus     status;
            bool             used;
            bool             dirty;
        };

        Entry    entries[CLIENT_TRACKER_MAX];
        uint32_t lastTickMs;
        uint32_t lastSaveMs;
        uint32_t lastMetricsMs;
        uint32_t lastPurgeRemoved;
        bool     registryDirty;

        int  findOrAlloc(const uint8_t* mac);
        int  findEntry(const uint8_t* mac) const;
        int  findAccesspoint(const uint8_t* apMac);
        void touchEntry(int idx, int8_t rssi, ClientDetSource type);
        void tryPair(const uint8_t* a, const uint8_t* b, int8_t rssi, ClientDetSource type);
        void fromMgmt(uint8_t* buf, uint16_t len, int8_t rssi);
        void purgeStale();
        void applySnapshotToStations();
        bool registryHasMac(const uint8_t* mac) const;
};

extern ClientTracker clientTracker;
