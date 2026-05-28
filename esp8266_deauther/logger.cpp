/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

#include "A_config.h"
#include "logger.h"

#include <stdarg.h>
#include <stdio.h>

extern "C" {
#include "user_interface.h"
}

#include "wifi.h"
#include "Scan.h"
#include "HandshakeCapture.h"
#include "client_tracker.h"

extern Scan             scan;
extern uint32_t         currentTime;

namespace {

#if DIAG_ENABLE

static uint32_t s_loopId           = 0;
static uint32_t s_lastSnapshotMs   = 0;
static uint32_t s_lastMemMs        = 0;
static uint32_t s_lastPromiscSumMs = 0;
static uint16_t s_lastPromiscDrops = 0;

static const uint8_t PROMISC_EVT_RING = 48;
static volatile uint8_t promisc_evt_head = 0;
static volatile uint8_t promisc_evt_tail = 0;
static volatile uint8_t promisc_evt_buf[PROMISC_EVT_RING];

static const char* levelStr(DiagLevel level) {
    switch (level) {
        case DIAG_DEBUG: return "DEBUG";
        case DIAG_INFO:  return "INFO";
        case DIAG_WARN:  return "WARN";
        case DIAG_ERROR: return "ERROR";
        default:         return "?";
    }
}

static const char* phaseStr(DiagPhase phase) {
    switch (phase) {
        case DIAG_P0: return "P0";
        case DIAG_P1: return "P1";
        case DIAG_P2: return "P2";
        case DIAG_P3: return "P3";
        case DIAG_P4: return "P4";
        default:      return "P?";
    }
}

static bool levelEnabled(DiagLevel level) {
    return (uint8_t)level >= (uint8_t)DIAG_LOG_LEVEL;
}

static void writeLine(DiagLevel level, const char* module, const char* body, bool withPhase,
                      DiagPhase phase) {
    if (!levelEnabled(level)) return;

    char line[160];
    int  n;

    if (withPhase) {
        n = snprintf(line, sizeof(line), "[loop=%lu][%s][%s][%s] %s", (unsigned long)s_loopId,
                     phaseStr(phase), levelStr(level), module ? module : "?", body ? body : "");
    } else {
        n = snprintf(line, sizeof(line), "[loop=%lu][%s][%s] %s", (unsigned long)s_loopId,
                     levelStr(level), module ? module : "?", body ? body : "");
    }

    if (n < 0) return;

    if ((size_t)n >= sizeof(line)) {
        line[sizeof(line) - 2] = '+';
        line[sizeof(line) - 1] = '\0';
    }

    Serial.println(line);
}

static const char* resetReasonStr() {
    const rst_info* info = system_get_rst_info();

    if (!info) return "unknown";

    switch (info->reason) {
        case REASON_DEFAULT_RST:      return "default";
        case REASON_WDT_RST:          return "wdt";
        case REASON_EXCEPTION_RST:    return "exception";
        case REASON_SOFT_WDT_RST:     return "soft_wdt";
        case REASON_SOFT_RESTART:     return "soft_restart";
        case REASON_DEEP_SLEEP_AWAKE: return "deep_sleep";
        case REASON_EXT_SYS_RST:      return "ext";
        default:                      return "other";
    }
}

static uint32_t freeContStack() {
#if defined(ESP8266)
    return ESP.getFreeContStack();
#else
    return 0;
#endif
}

static const char* scanModeLabel() {
    if (scan.isPassiveApScan()) return "passive_ap";

    if (scan.isScanNetworksActive()) return "scanNetworks";

    if (hsCaptureActive()) return "handshake";

    if (scan.isSniffing()) return "sniff";

    if (scan.isScanning()) return "scan";

    return "idle";
}

#endif // DIAG_ENABLE

} // namespace

namespace diag {

void begin() {
#if DIAG_ENABLE
    s_loopId           = 0;
    s_lastSnapshotMs   = 0;
    s_lastMemMs        = 0;
    s_lastPromiscSumMs = 0;
    s_lastPromiscDrops = wifi::promiscQueueDrops();
    promisc_evt_head   = 0;
    promisc_evt_tail   = 0;

    log(DIAG_INFO, "DIAG", "observability layer ready");
    memSnapshot("boot");
    logf(DIAG_INFO, "DIAG", "reset_reason=%s", resetReasonStr());
#endif
}

void loopBegin() {
#if DIAG_ENABLE
    s_loopId++;
#endif
}

uint32_t loopId() {
#if DIAG_ENABLE
    return s_loopId;
#else
    return 0;
#endif
}

void log(DiagLevel level, const char* module, const char* msg) {
#if DIAG_ENABLE
    writeLine(level, module, msg, false, DIAG_P0);
#else
    (void)level;
    (void)module;
    (void)msg;
#endif
}

void logf(DiagLevel level, const char* module, const char* fmt, ...) {
#if DIAG_ENABLE
    if (!levelEnabled(level) || !fmt) return;

    char body[128];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    writeLine(level, module, body, false, DIAG_P0);
#else
    (void)level;
    (void)module;
    (void)fmt;
#endif
}

void phase(DiagPhase phase, const char* module, const char* event) {
#if DIAG_ENABLE
    if (!levelEnabled(DIAG_DEBUG)) return;
    writeLine(DIAG_DEBUG, module, event ? event : "", true, phase);
#else
    (void)phase;
    (void)module;
    (void)event;
#endif
}

void promiscPush(DiagPromiscEvt evt) {
#if DIAG_ENABLE
    uint8_t next = (promisc_evt_head + 1) % PROMISC_EVT_RING;

    if (next == promisc_evt_tail) return; // overflow: drop diag event (not frame)

    promisc_evt_buf[promisc_evt_head] = (uint8_t)evt;
    promisc_evt_head                  = next;
#else
    (void)evt;
#endif
}

void drainPromiscEvents() {
#if DIAG_ENABLE
    uint32_t ok = 0, fail = 0, overflow = 0;

    while (promisc_evt_tail != promisc_evt_head) {
        uint8_t code = promisc_evt_buf[promisc_evt_tail];
        promisc_evt_tail = (promisc_evt_tail + 1) % PROMISC_EVT_RING;

        if (code == (uint8_t)DIAG_PROMISC_ENQ_OK) ok++;
        else if (code == (uint8_t)DIAG_PROMISC_ENQ_FAIL) fail++;
        else overflow++;
    }

    if (ok == 0 && fail == 0) return;

    if (currentTime - s_lastPromiscSumMs < DIAG_PROMISC_SUMMARY_MS) return;

    s_lastPromiscSumMs = currentTime;

    uint16_t drops = wifi::promiscQueueDrops();
    uint16_t delta = drops - s_lastPromiscDrops;
    s_lastPromiscDrops = drops;

    logf(DIAG_DEBUG, "PROMISC",
         "enq_ok=%lu enq_fail=%lu ring=%u/%u drops_total=%u drops_delta=%u",
         (unsigned long)ok, (unsigned long)fail, (unsigned)wifi::promiscRingDepth(),
         (unsigned)wifi::promiscRingCapacity(), (unsigned)drops, (unsigned)delta);
#else
#endif
}

void httpTrace(const char* path, uint16_t statusCode) {
#if DIAG_ENABLE
    if (!levelEnabled(DIAG_INFO)) return;

    char body[96];
    snprintf(body, sizeof(body), "%s -> %u", path ? path : "?", (unsigned)statusCode);
    writeLine(DIAG_INFO, "HTTP", body, false, DIAG_P0);
#else
    (void)path;
    (void)statusCode;
#endif
}

void memSnapshot(const char* reason) {
#if DIAG_ENABLE
    if (!levelEnabled(DIAG_INFO)) return;

    logf(DIAG_INFO, "MEM", "%s heap=%u frag=%u%% cont_stack=%u", reason ? reason : "snap",
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapFragmentation(),
         (unsigned)freeContStack());
#else
    (void)reason;
#endif
}

void systemSnapshot() {
#if DIAG_ENABLE
    logf(DIAG_INFO, "SNAP",
         "ap=%u sta_sniff=%u mon=%u scan=%s clients=%lu ring=%u/%u drops=%u heap=%u",
         (unsigned)(wifi::isApActive() ? 1 : 0), (unsigned)(wifi::isStationSniffMode() ? 1 : 0),
         (unsigned)(wifi::isMonitorActive() ? 1 : 0), scanModeLabel(), (unsigned long)clientTracker.count(),
         (unsigned)wifi::promiscRingDepth(), (unsigned)wifi::promiscRingCapacity(),
         (unsigned)wifi::promiscQueueDrops(), (unsigned)ESP.getFreeHeap());
#else
#endif
}

void tick() {
#if DIAG_ENABLE
    if (currentTime - s_lastMemMs >= DIAG_MEM_MS) {
        s_lastMemMs = currentTime;
        memSnapshot("periodic");
    }

    if (currentTime - s_lastSnapshotMs >= DIAG_SNAPSHOT_MS) {
        s_lastSnapshotMs = currentTime;
        systemSnapshot();
    }
#else
#endif
}

} // namespace diag
