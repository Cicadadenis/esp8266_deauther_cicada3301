/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

#pragma once

#include "Arduino.h"

namespace wifi {
    void begin();

    String getMode();
    void printStatus();

    void startNewAP(String path, String ssid, String password, uint8_t ch, bool hidden);
    void startAP(bool backgroundApScan = true);

    bool isApActive();

    void stopAP();
    void resumeAP();

    void syncCaptivePortal();

    // P0: HTTP + DNS + P1: drain promisc ring (call every loop iteration)
    void update();

    // P2: registry maintenance
    void tickClients();

    // ISR/callback entry — enqueue only (no heap, no tracker)
    void onPromiscuousRx(uint8_t* buf, uint16_t len);
    void promiscRxFromIsr(uint8_t* buf, uint16_t len);

    void installPromiscCallback();
    bool isRepeaterWorkmode();
    void enableMonitorMode();
    void suspendMonitorMode();
    // Pause softAP for promiscuous station scan (original deauther stopAP path)
    void pauseApForSniff(uint8_t ch);
    void restoreApAfterSniff();
    void enterStationSniffMode(uint8_t ch);
    void leaveStationSniffMode();
    bool isStationSniffMode();
    void setMonitorChannel(uint8_t ch);
    bool isMonitorActive();

    uint16_t promiscQueueDrops();

    uint8_t promiscRingDepth();
    uint8_t promiscRingCapacity();
    uint32_t promiscIsrCount();
}
