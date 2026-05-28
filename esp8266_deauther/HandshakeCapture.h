/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

#pragma once

#include "Arduino.h"

#define HS_MAX_FRAME   400
#define HS_FILE_CAP    "/handshake.cap"
#define HS_FILE_STATUS "/handshake.json"

class HandshakeCapture {
    public:
        HandshakeCapture();

        void start(int apId, int stId, uint32_t timeoutMs);
        void start(int apId, const uint8_t* stMac, uint32_t timeoutMs);
        void onSniffEnded();
        void stop(bool save);
        void update();
        void onFrame(uint8_t* buf, uint16_t len);

        bool isActive();
        bool hasHandshake();
        uint8_t getMsgMask() const;
        uint32_t getFramesSeen() const;
        uint32_t getFramesData() const;
        uint32_t getEapolFound() const;
        String getStatusJson();

    private:
        struct Slot {
            bool     used;
            uint8_t  msgNum;
            uint16_t len;
            uint8_t  data[HS_MAX_FRAME];
        };

        bool     active;
        int      apId;
        int      stId;
        bool     hasSt;
        uint8_t  apMac[6];
        uint8_t  stMac[6];
        uint8_t  channel;
        String   ssid;

        uint32_t startTime;
        uint32_t timeout;
        uint32_t lastDeauth;
        uint8_t  msgMask;
        uint32_t framesSeen;   // all promisc frames passed to onFrame
        uint32_t framesData;   // 802.11 Data / QoS-Data only
        uint32_t framesMatch;  // AP (+ target STA) filter passed
        uint32_t eapolFound;   // findEapol() matched
        uint32_t eapolBad;     // EAPOL found but getEapolMsgNum()==0
        uint8_t  dbgDataLogs;
        uint8_t  dbgEapolLogs;
        uint8_t  dbgMatchDump;
        uint32_t deauthCount;

        Slot slots[4];

        bool   macEq(uint8_t* a, uint8_t* b);
        bool   frameHasAp(uint8_t* buf, uint16_t len);
        void   dumpMatchFrame(uint8_t* buf, uint16_t len);
        bool   findEapol(uint8_t* buf, uint16_t len, uint16_t& eapolOff, uint16_t& eapolLen);
        uint8_t getEapolMsgNum(uint8_t* eapol, uint16_t len);
        void   storeSlot(uint8_t msgNum, uint8_t* buf, uint16_t len);
        void   triggerDeauth();
        bool   savePcap();
        void   saveStatus();
};

extern HandshakeCapture* handshakeCapture;

/** Lazy instance: nullptr until capture start; freed after stop / sniff end. */
bool hsCaptureActive();

void hsCaptureUpdate();

void hsCaptureOnFrame(uint8_t* buf, uint16_t len);

void hsCaptureOnSniffEnded();

/** Safe readers — null-checked; use instead of handshakeCapture-> outside this module. */
uint32_t hsGetFramesSeen();
uint32_t hsGetFramesData();
uint32_t hsGetEapolFound();
uint32_t hsGetMsgMask();

/** Status JSON (active session or last /handshake.json on disk). */
String hsGetStatusJson();

void hsCaptureStart(int apId, int stId, uint32_t timeoutMs);
void hsCaptureStart(int apId, const uint8_t* stMac, uint32_t timeoutMs);
void hsCaptureStop(bool save);
