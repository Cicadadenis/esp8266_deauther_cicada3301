/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

#pragma once

#include <Arduino.h>
#include <stdint.h>

// Observability layer — compile-time defaults (override in A_config.h if needed)
#ifndef DIAG_ENABLE
#define DIAG_ENABLE 1
#endif

#ifndef DIAG_LOG_LEVEL
#define DIAG_LOG_LEVEL 1 // 0=DEBUG 1=INFO 2=WARN 3=ERROR
#endif

#ifndef DIAG_SNAPSHOT_MS
#define DIAG_SNAPSHOT_MS 20000
#endif

#ifndef DIAG_MEM_MS
#define DIAG_MEM_MS 10000
#endif

#ifndef DIAG_PROMISC_SUMMARY_MS
#define DIAG_PROMISC_SUMMARY_MS 3000
#endif

#ifndef DIAG_TRACKER_METRICS_MS
#define DIAG_TRACKER_METRICS_MS 3000
#endif

enum DiagLevel : uint8_t {
    DIAG_DEBUG = 0,
    DIAG_INFO  = 1,
    DIAG_WARN  = 2,
    DIAG_ERROR = 3
};

enum DiagPhase : uint8_t {
    DIAG_P0 = 0, // AP / HTTP / DNS
    DIAG_P1 = 1, // promiscuous drain
    DIAG_P2 = 2, // client tracker
    DIAG_P3 = 3, // scan
    DIAG_P4 = 4  // handshake
};

enum DiagPromiscEvt : uint8_t {
    DIAG_PROMISC_ENQ_OK   = 1,
    DIAG_PROMISC_ENQ_FAIL = 2
};

namespace diag {

void begin();

void loopBegin();
uint32_t loopId();

void log(DiagLevel level, const char* module, const char* msg);
void logf(DiagLevel level, const char* module, const char* fmt, ...);
void phase(DiagPhase phase, const char* module, const char* event);

// ISR/callback-safe: single-byte event enqueue (no strings, no heap)
void promiscPush(DiagPromiscEvt evt);
void drainPromiscEvents();

void httpTrace(const char* path, uint16_t statusCode);

void memSnapshot(const char* reason);
void systemSnapshot();

void tick();

} // namespace diag

#if DIAG_ENABLE

#define DIAG_LOG(l, m, msg)  diag::log((l), (m), (msg))
#define DIAG_LOGF(l, m, ...) diag::logf((l), (m), __VA_ARGS__)
#define DIAG_PHASE(p, m, e)  diag::phase((p), (m), (e))
#define DIAG_HTTP(p, c)      diag::httpTrace((p), (c))

#else

#define DIAG_LOG(l, m, msg)  ((void)0)
#define DIAG_LOGF(l, m, ...) ((void)0)
#define DIAG_PHASE(p, m, e)  ((void)0)
#define DIAG_HTTP(p, c)      ((void)0)

#endif
