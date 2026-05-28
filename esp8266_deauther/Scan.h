/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

#pragma once

#include "Arduino.h"
#include "Accesspoints.h"
#include "Stations.h"
#include "Names.h"
#include "SSIDs.h"
#include "language.h"
#include "SimpleList.h"

#define SCAN_MODE_OFF 0
#define SCAN_MODE_APS 1
#define SCAN_MODE_STATIONS 2
#define SCAN_MODE_ALL 3
#define SCAN_MODE_SNIFFER 4
#define SCAN_DEFAULT_TIME 15000
#define SCAN_DEFAULT_CONTINUE_TIME 10000
#define SCAN_PACKET_LIST_SIZE 64

extern Accesspoints accesspoints;
extern Stations     stations;
extern Names names;
extern SSIDs ssids;

extern uint8_t wifiMode;

extern void setWifiChannel(uint8_t ch, bool force);
extern bool appendFile(String path, String& buf);
extern bool writeFile(String path, String& buf);
extern void readFileToSerial(const String path);
extern String escape(String str);

class Scan {
    public:
        Scan();

        void sniffer(uint8_t* buf, uint16_t len);
        void start(uint8_t mode, uint32_t time, uint8_t nextmode, uint32_t continueTime, bool channelHop, uint8_t channel);
        void start(uint8_t mode);

        void setup();
        void update();
        void stop();
        void save(bool force);
        void save(bool force, String filePath);
        /** No-op when AP/ST lists unchanged (for UI `save scan` polling). */
        void saveScanIfNeeded();

        /** RAM cache for GET /scan.json (avoids LittleFS read on poll). */
        bool getJsonCache(String& out) const;
        /** Zero-copy view; invalid when AP/ST/sniff state is stale. */
        bool peekJsonCache(const String*& out) const;
        bool hasJsonCache() const;
        uint32_t getJsonVersion() const;

        void selectAll();
        void deselectAll();
        void printAll();
        void printSelected();

        uint8_t getPercentage();
        uint32_t getPackets(int i);
        uint32_t countAll();
        uint32_t countSelected();
        bool isScanning();
        bool isSniffing();
        bool isScanNetworksActive();
        bool isPassiveApScan() const;
        void onPromiscFrame(uint8_t* buf, uint16_t len);

        void nextChannel();
        void setChannel(uint8_t newChannel);

        String getMode();
        double getScaleFactor(uint8_t height);
        uint32_t getMaxPacket();
        uint32_t getPacketRate();

        uint16_t deauths = 0;
        uint16_t packets = 0;
        uint32_t sniffTotal = 0;

    private:
        uint32_t stationKickTime = 0;

        void refreshApsForStScan(uint8_t channel, bool channelHop);
        SimpleList<uint16_t>* list;                      // packet list

        uint32_t sniffTime          = SCAN_DEFAULT_TIME; // how long the scan runs
        uint32_t snifferStartTime   = 0;                 // when the scan started
        uint32_t snifferOutputTime  = 0;                 // last info output (every 3s)
        uint32_t snifferChannelTime = 0;                 // last time the channel was changed
        uint32_t snifferPacketTime  = 0;                 // last time the packet rate was reseted (every 1s)

        uint8_t scanMode = 0;

        uint8_t scan_continue_mode = 0;                          // restart mode after scan stopped
        uint32_t continueTime      = SCAN_DEFAULT_CONTINUE_TIME; // time in ms to wait until scan restarts
        uint32_t continueStartTime = 0;                          // when scan restarted

        bool channelHop            = true;
        bool apStoppedForScan      = false;
        bool sniffKeepsAp          = false;
        bool scan_networks_active  = false;
        bool passive_ap_scan       = false;
        uint32_t passive_ap_hop_ms = 0;
        uint16_t tmpDeauths        = 0;

        static const uint32_t PASSIVE_AP_SCAN_MS = 14000;
        static const uint32_t PASSIVE_AP_HOP_MS  = 300;
        uint32_t passive_ap_save_ms = 0;

        void finishPassiveApScan();
        void parseBeacon(uint8_t* buf, uint16_t len, int8_t rssi);
        bool apWithChannel(uint8_t ch);
        int findAccesspoint(uint8_t* mac);
        void tryAddStation(uint8_t* staMac, uint8_t* apMac);
        void tryAddStationPair(uint8_t* a, uint8_t* b);
        void snifferFromMgmt(uint8_t* buf, uint16_t len);

        String FILE_PATH = "/scan.json";

        String   jsonCache;
        bool     jsonCacheValid   = false;
        uint32_t jsonVersion      = 0;
        bool     scanDirty        = true;
        uint32_t sniffTotalAtSave = 0;
};