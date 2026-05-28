/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

#include "wifi.h"

extern "C" {
    #include "user_interface.h"
}

#if LWIP_FEATURES && !LWIP_IPV6
    #include <lwip/napt.h>
    #include <lwip/dns.h>
    #define REPEATER_NAPT_AVAILABLE 1
#else
    #define REPEATER_NAPT_AVAILABLE 0
#endif

#ifndef OFFER_DNS
    #define OFFER_DNS 0x02
#endif

#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <FS.h>

#include "A_config.h"
#include "language.h"
#include "debug.h"
#include "settings.h"
#include "CLI.h"
#include "Attack.h"
#include "Scan.h"
#include "HandshakeCapture.h"
#include "client_tracker.h"
#include "logger.h"
#include "firmware_update.h"
#include "led.h"

extern uint32_t         currentTime;
extern bool progmemToSpiffs(const char* adr, int len, String path);

#include "webfiles.h"
#include "src/ArduinoJson-v5.13.5/ArduinoJson.h"

extern bool readFile(String path, String& buf);
extern bool writeFile(String path, String& buf);
extern JsonVariant parseJSONFile(String path, DynamicJsonBuffer& jsonBuffer);

extern Scan   scan;
extern CLI    cli;
extern Attack attack;

extern "C" void promisc_rx_cb(uint8_t* buf, uint16_t len);

typedef enum wifi_mode_t {
    off = 0,
    ap  = 1,
    st  = 2
} wifi_mode_t;

typedef struct ap_settings_t {
    char    path[33];
    char    ssid[33];
    char    password[65];
    uint8_t channel;
    bool    hidden;
    bool    captive_portal;
} ap_settings_t;

namespace wifi {
    // ===== PRIVATE ===== //
    wifi_mode_t   mode;
    ap_settings_t ap_settings;
    // Server and other global objects
    ESP8266WebServer server(80);
    DNSServer dns;
    bool      dns_captive_active = false;
    bool      monitor_active     = false;
    IPAddress ip WEB_IP_ADDR;

    // Promiscuous RX ring (SPSC: callback producer, main loop consumer)
    static const uint8_t  PROMISC_RING_SLOTS = 24;
    static const uint16_t PROMISC_FRAME_MAX  = 256;
    static const uint8_t  PROMISC_DRAIN_MAX  = 12;

    struct PromiscSlot {
        uint16_t len;
        uint8_t  flags; // bit0: route to scan.sniffer (P4 session)
        uint8_t  data[PROMISC_FRAME_MAX];
    };

    static PromiscSlot     promisc_ring[PROMISC_RING_SLOTS];
    static volatile uint8_t promisc_head  = 0;
    static volatile uint8_t promisc_tail  = 0;
    static volatile uint16_t promisc_drops = 0;

    static bool station_sniff_mode = false;
    static volatile uint32_t promisc_isr_count = 0;
    static volatile uint8_t  promisc_sniff_path = 0;

    static bool     repeater_active = false;
    static bool     repeater_napt_enabled = false;
    static bool     repeater_napt_inited  = false;
    static String   repeater_up_ssid;
    static String   repeater_ap_ssid;

    // SoftAP DHCP always advertises AP IP as DNS; relay queries to upstream in repeater mode.
    static WiFiUDP  repeater_dns_listen;
    static WiFiUDP  repeater_dns_upstream;
    static IPAddress repeater_dns_target;
    static bool     repeater_dns_forward_active = false;
    static uint8_t  repeater_dns_fwd_buf[512];
    static int      repeater_dns_fwd_len        = 0;
    static IPAddress repeater_dns_fwd_client_ip;
    static uint16_t repeater_dns_fwd_client_port = 0;
    static uint32_t repeater_dns_fwd_start       = 0;
    static bool     repeater_dns_fwd_waiting     = false;

    static const char* REPEATER_CFG_PATH = "/repeater.json";
    static bool        repeater_boot_tried = false;

    enum class RepeaterAsyncPhase : uint8_t {
        Idle,
        Suspend,
        Delay,
        TestBegin,
        TestWait,
        BridgeModeOff,
        BridgeApCreate,
        BridgeStaBegin,
        BridgeStaWait,
        BridgeApRetune,
        BridgeFinish,
    };

    static RepeaterAsyncPhase repeater_async_phase = RepeaterAsyncPhase::Idle;
    static bool               repeater_async_test  = false;
    static bool               repeater_async_ok    = false;
    static bool               repeater_async_reboot_after_finish = false;
    static uint32_t           repeater_async_t0    = 0;
    static uint32_t           repeater_async_log     = 0;
    static String             repeater_async_up_ssid;
    static String             repeater_async_up_pass;
    static String             repeater_async_ap_ssid;
    static String             repeater_async_ap_pass;

    static void repeaterAsyncReset() {
        repeater_async_phase = RepeaterAsyncPhase::Idle;
        repeater_async_test  = false;
    }

    static void repeaterAsyncPollHttp() {
        server.handleClient();

        if (dns_captive_active) dns.processNextRequest();
    }

    static void beginRepeaterBridgeAsync(const String& upSsid, const String& upPass, const String& apSsid,
                                         const String& apPass, bool rebootAfterFinish);

    static void repeaterAsyncTick();

    // Captive portal / auth switches (stored in LittleFS, not EEPROM)
    static const char* PORTAL_PREFS_PATH = "/portal_prefs.json";
    static bool        portal_pref_captive = true;
    static bool        portal_pref_auth    = true;

    struct repeater_saved_cfg_t {
        bool   enabled;
        String upSsid;
        String upPass;
        String apSsid;
        String apPass;
    };

    static bool loadRepeaterConfig(repeater_saved_cfg_t& cfg) {
        cfg.enabled = false;
        cfg.upSsid  = "";
        cfg.upPass  = "";
        cfg.apSsid  = "";
        cfg.apPass  = "";

        if (!LittleFS.exists(REPEATER_CFG_PATH)) return false;

        DynamicJsonBuffer jsonBuffer(640);
        JsonVariant parsed = parseJSONFile(String(REPEATER_CFG_PATH), jsonBuffer);

        if (!parsed.success()) return false;

        JsonObject& root = parsed.as<JsonObject>();

        cfg.enabled = root["enabled"] | false;
        if (root.containsKey("upSsid")) cfg.upSsid = root["upSsid"].as<String>();
        if (root.containsKey("upPass")) cfg.upPass = root["upPass"].as<String>();
        if (root.containsKey("apSsid")) cfg.apSsid = root["apSsid"].as<String>();
        if (root.containsKey("apPass")) cfg.apPass = root["apPass"].as<String>();

        cfg.upSsid.trim();
        return cfg.enabled && cfg.upSsid.length() > 0;
    }

    static bool saveRepeaterConfig(const String& upSsid, const String& upPass, const String& apSsid,
                                   const String& apPass) {
        DynamicJsonBuffer jsonBuffer(640);
        JsonObject& root = jsonBuffer.createObject();

        root["enabled"] = true;
        root["upSsid"]  = upSsid;
        root["upPass"]  = upPass;
        root["apSsid"]  = apSsid;
        root["apPass"]  = apPass;

        String buf;
        root.printTo(buf);

        if (!writeFile(String(REPEATER_CFG_PATH), buf)) {
            prntln("[Repeater] ERROR: failed to save " + String(REPEATER_CFG_PATH));
            return false;
        }

        prntln("[Repeater] Config saved to " + String(REPEATER_CFG_PATH));
        return true;
    }

    static void clearRepeaterConfig() {
        if (LittleFS.exists(REPEATER_CFG_PATH)) {
            LittleFS.remove(REPEATER_CFG_PATH);
            prntln("[Repeater] Config removed");
        }
    }

    static void tryAutoStartRepeater() {
        if (repeater_active || repeater_boot_tried) return;
        if (!isRepeaterWorkmode()) return;
        if (scan.isScanning()) return;

        repeater_boot_tried = true;

        repeater_saved_cfg_t cfg;

        if (!loadRepeaterConfig(cfg)) {
            prntln("[Repeater] No saved upstream — select network and press «Применить» (step 3)");
            return;
        }

        const access_point_settings_t& savedAp = settings::getAccessPointSettings();
        const String apSsid = cfg.apSsid.length() > 0 ? cfg.apSsid : String(savedAp.ssid);
        const String apPass = cfg.apPass.length() > 0 ? cfg.apPass : String(savedAp.password);

        prntln("[Repeater] Auto-starting bridge to «" + cfg.upSsid + "»…");
        // Boot auto-start must not reboot on success, otherwise it loops forever.
        beginRepeaterBridgeAsync(cfg.upSsid, cfg.upPass, apSsid, apPass, false);
    }

    static void stopRepeaterDnsForwarder() {
        repeater_dns_listen.stop();
        repeater_dns_upstream.stop();
        repeater_dns_forward_active = false;
        repeater_dns_fwd_waiting    = false;
        repeater_dns_fwd_len        = 0;
    }

    static void startRepeaterDnsForwarder(const IPAddress& upstreamDns) {
        stopRepeaterDnsForwarder();

        repeater_dns_target = upstreamDns;
        if (repeater_dns_target == IPAddress(0, 0, 0, 0)) {
            repeater_dns_target = IPAddress(8, 8, 8, 8);
        }

        if (repeater_dns_listen.begin(53)) {
            repeater_dns_forward_active = true;
            prntln("[Repeater] DNS forwarder on :53 -> " + repeater_dns_target.toString());
        } else {
            prntln("[Repeater] DNS forwarder FAILED to bind :53");
        }
    }

    static void processRepeaterDnsForwarder() {
        if (!repeater_dns_forward_active) return;

        if (repeater_dns_fwd_waiting) {
            const int r = repeater_dns_upstream.parsePacket();
            if (r > 0) {
                const int rlen = repeater_dns_upstream.read(repeater_dns_fwd_buf, sizeof(repeater_dns_fwd_buf));
                if (rlen > 0) {
                    repeater_dns_listen.beginPacket(repeater_dns_fwd_client_ip, repeater_dns_fwd_client_port);
                    repeater_dns_listen.write(repeater_dns_fwd_buf, (size_t)rlen);
                    repeater_dns_listen.endPacket();
                }
                repeater_dns_fwd_waiting = false;
            } else if (millis() - repeater_dns_fwd_start > 800) {
                repeater_dns_fwd_waiting = false;
            }
            return;
        }

        const int packetSize = repeater_dns_listen.parsePacket();
        if (packetSize <= 0) return;

        repeater_dns_fwd_len = repeater_dns_listen.read(repeater_dns_fwd_buf, sizeof(repeater_dns_fwd_buf));
        if (repeater_dns_fwd_len < 12) return;

        repeater_dns_fwd_client_ip   = repeater_dns_listen.remoteIP();
        repeater_dns_fwd_client_port = repeater_dns_listen.remotePort();

        repeater_dns_upstream.begin(0);
        repeater_dns_upstream.beginPacket(repeater_dns_target, 53);
        repeater_dns_upstream.write(repeater_dns_fwd_buf, (size_t)repeater_dns_fwd_len);
        repeater_dns_upstream.endPacket();

        repeater_dns_fwd_waiting = true;
        repeater_dns_fwd_start   = millis();
    }

    static void disableRepeaterNapt() {
#if REPEATER_NAPT_AVAILABLE
        if (repeater_napt_enabled) {
            ip_napt_enable_no(SOFTAP_IF, 0);
            repeater_napt_enabled = false;
            prntln("[Repeater] NAPT disabled");
        }
#endif
        stopRepeaterDnsForwarder();
    }

    static void configureRepeaterApDns(const IPAddress& upstreamDns) {
        IPAddress dnsToUse = upstreamDns;
        if (dnsToUse == IPAddress(0, 0, 0, 0)) {
            // Some upstreams don't provide DNS via DHCP; use a public fallback so clients have working name resolution.
            dnsToUse = IPAddress(8, 8, 8, 8);
            prntln("[Repeater] DNS: upstream missing, fallback to 8.8.8.8");
        } else {
            prntln("[Repeater] DNS: upstream " + dnsToUse.toString());
        }

#if REPEATER_NAPT_AVAILABLE
        ip_addr_t dnsAddr;
        ip4_addr_set_u32(ip_2_ip4(&dnsAddr), static_cast<uint32_t>(dnsToUse));
        dns_setserver(0, &dnsAddr);
#endif

        // SDK 2.7.5: нет softAPDhcpServer — включаем DNS в DHCP через user_interface
        wifi_softap_dhcps_stop();
        uint8_t offerDns = 1;
        wifi_softap_set_dhcps_offer_option(OFFER_DNS, &offerDns);
        wifi_softap_dhcps_start();

        // Clients use AP IP (192.168.4.1) as DNS — forward UDP/53 to upstream resolver.
        startRepeaterDnsForwarder(dnsToUse);
    }

    static void enableRepeaterNapt() {
#if REPEATER_NAPT_AVAILABLE
        prntln("[Repeater] NAPT: Checking STA connection...");
        if (WiFi.status() != WL_CONNECTED) {
            prntln("[Repeater] NAPT: ERROR - STA not connected!");
            return;
        }

        prntln("[Repeater] NAPT: Configuring DNS from upstream...");
        configureRepeaterApDns(WiFi.dnsIP(0));

        if (!repeater_napt_inited) {
            // Default values can be too large for low-heap builds; try smaller tables first.
            struct NaptCfg { uint16_t ports; uint16_t pcbs; };
            const NaptCfg tries[] = {
                {128, 16},
                {256, 16},
                {256, 24},
                {512, 32},
            };

            bool ok = false;
            for (unsigned i = 0; i < (sizeof(tries) / sizeof(tries[0])); i++) {
                prntln("[Repeater] NAPT: Initializing (" + String(tries[i].ports) + " ports, " + String(tries[i].pcbs) + " PCBs)...");
                if (ip_napt_init(tries[i].ports, tries[i].pcbs) == ERR_OK) {
                    ok = true;
                    break;
                }
                yield();
                delay(20);
            }

            if (!ok) {
                prntln("[Repeater] NAPT: ERROR - init failed (heap too low?)");
                return;
            }

            repeater_napt_inited = true;
            prntln("[Repeater] NAPT: Init success");
        }

        prntln("[Repeater] NAPT: Enabling on SoftAP interface...");
        if (ip_napt_enable_no(SOFTAP_IF, 1) == ERR_OK) {
            repeater_napt_enabled = true;
            prntln("[Repeater] NAPT: SUCCESS - relay active");
        } else {
            prntln("[Repeater] NAPT: ERROR - enable failed");
        }
#else
        prntln("[Repeater] NAPT not available in this build");
#endif
    }

    static bool promiscEnqueue(uint8_t* buf, uint16_t len, bool sniffPath) {
        if (!buf || len < 12) return false;

        uint8_t next = (promisc_head + 1) % PROMISC_RING_SLOTS;

        if (next == promisc_tail) {
            promisc_drops++;
            diag::promiscPush(DIAG_PROMISC_ENQ_FAIL);
            return false;
        }

        PromiscSlot& slot = promisc_ring[promisc_head];
        uint16_t     copy = len > PROMISC_FRAME_MAX ? PROMISC_FRAME_MAX : len;

        slot.len   = copy;
        slot.flags = sniffPath ? 1 : 0;
        memcpy(slot.data, buf, copy);

        promisc_head = next;
        diag::promiscPush(DIAG_PROMISC_ENQ_OK);
        return true;
    }

    void ICACHE_RAM_ATTR promiscRxFromIsr(uint8_t* buf, uint16_t len) {
        if (!buf || len < 12) return;

        promisc_isr_count++;

        uint8_t next = (promisc_head + 1) % PROMISC_RING_SLOTS;

        if (next == promisc_tail) {
            promisc_drops++;
            return;
        }

        PromiscSlot& slot = promisc_ring[promisc_head];
        uint16_t     copy = len > PROMISC_FRAME_MAX ? PROMISC_FRAME_MAX : len;

        slot.len   = copy;
        slot.flags = promisc_sniff_path;
        memcpy(slot.data, buf, copy);
        promisc_head = next;
    }

    static uint8_t promiscRingDepthInternal() {
        uint8_t head = promisc_head;
        uint8_t tail = promisc_tail;

        if (head >= tail) return head - tail;

        return PROMISC_RING_SLOTS - tail + head;
    }

    void installPromiscCallback() {
        wifi_set_promiscuous_rx_cb(promisc_rx_cb);
    }

    static void enablePromiscuousRx(bool resetRing) {
        installPromiscCallback();

        if (resetRing) {
            promisc_head  = 0;
            promisc_tail  = 0;
            promisc_drops = 0;
        }

        wifi_promiscuous_enable(0);
        yield();
        wifi_promiscuous_enable(1);
        monitor_active = true;
    }

    static void processPromiscQueue() {
        uint8_t budget = (scan.isSniffing() || hsCaptureActive()) ? PROMISC_RING_SLOTS : PROMISC_DRAIN_MAX;
        bool    throttleTracker = scan.isScanNetworksActive();
        uint8_t throttleCnt     = 0;

        while (budget > 0 && promisc_tail != promisc_head) {
            PromiscSlot& slot = promisc_ring[promisc_tail];
            bool         sniffPath = (slot.flags & 1) != 0;

            if (hsCaptureActive()) {
                hsCaptureOnFrame(slot.data, slot.len);
            } else if (scan.isSniffing()) {
                scan.sniffer(slot.data, slot.len);
            }

            if (scan.isPassiveApScan()) scan.onPromiscFrame(slot.data, slot.len);

            bool processTracker = true;

            if (throttleTracker) {
                throttleCnt++;
                processTracker = ((throttleCnt & 3) == 0);
            }

            if (processTracker) clientTracker.onFrame(slot.data, slot.len);

            promisc_tail = (promisc_tail + 1) % PROMISC_RING_SLOTS;
            budget--;
        }

        diag::drainPromiscEvents();
    }

    IPAddress netmask(255, 255, 255, 0);

    String    portal_auth_password = "cicada3301";
    IPAddress portal_auth_ip;
    static uint8_t portal_auth_mac[6];
    static bool   portal_auth_mac_valid = false;
    static uint8_t ap_station_count_prev = 0;
    const char* AUTH_PASSWORD_PATH = "/auth_password.txt";

        static void clearPortalAuth() {
            portal_auth_ip        = IPAddress(0, 0, 0, 0);
            portal_auth_mac_valid = false;
        }

        static bool portalAuthStationStillConnected() {
            if (portal_auth_ip == IPAddress(0, 0, 0, 0)) return false;

            const uint8_t n = wifi_softap_get_station_num();
            if (n == 0) return false;

            if (!portal_auth_mac_valid) return true;

            struct station_info* st = wifi_softap_get_station_info();
            bool found            = false;

            while (st != NULL) {
                if (memcmp(st->bssid, portal_auth_mac, 6) == 0) {
                    found = true;
                    break;
                }
                st = STAILQ_NEXT(st, next);
            }

            wifi_softap_free_station_info();
            return found;
        }

        static void syncPortalAuthWithApStations() {
            if (portal_auth_password.length() == 0 || mode != wifi_mode_t::ap) return;

            const uint8_t n = wifi_softap_get_station_num();

            if (n == 0) {
                if (portal_auth_ip != IPAddress(0, 0, 0, 0)) clearPortalAuth();
                ap_station_count_prev = 0;
                return;
            }

            // Клиент отключился от AP и подключился снова — снова показать вход
            if (ap_station_count_prev == 0) clearPortalAuth();

            if (portal_auth_ip != IPAddress(0, 0, 0, 0) && !portalAuthStationStillConnected()) {
                clearPortalAuth();
            }

            ap_station_count_prev = n;
        }

        static void installPortalAuthStationHandlers() {
            WiFi.onSoftAPModeStationDisconnected([](const WiFiEventSoftAPModeStationDisconnected&) {
                clearPortalAuth();
                ap_station_count_prev = wifi_softap_get_station_num();
            });

            WiFi.onSoftAPModeStationConnected([](const WiFiEventSoftAPModeStationConnected&) {
                const uint8_t n = wifi_softap_get_station_num();
                if (ap_station_count_prev == 0) clearPortalAuth();
                ap_station_count_prev = n;
            });
        }
    // "Remember device" removed: always password-based auth

        static const char AUTH_PAGE_HTML[] PROGMEM = R"AUTHHTML(
<!doctype html>
<html lang="ru">
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <meta name="theme-color" content="#050a1b">
    <title>Cicada3301 • Вход</title>
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link href="https://fonts.googleapis.com/css2?family=JetBrains+Mono:wght@400;500;700&family=Orbitron:wght@500;700&display=swap" rel="stylesheet">
    <style>
        :root {
            --text: #f4f6ff;
            --muted: #9aa8d8;
            --gold: #d4af37;
            --accent: #00f0ff;
            --danger: #ff3366;
            --success: #00ff9d;
            --neon: rgba(0, 240, 255, 0.38);
        }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
            font-family: 'JetBrains Mono', monospace;
            color: var(--text);
            background:
                radial-gradient(900px 500px at 12% -8%, rgba(34, 85, 255, 0.22) 0%, transparent 58%),
                radial-gradient(760px 420px at 92% 6%, rgba(122, 92, 255, 0.20) 0%, transparent 55%),
                linear-gradient(180deg, #050a1b 0%, #070b22 42%, #050a1b 100%);
            padding: 18px;
            overflow: hidden;
        }
        #starsBg { position: fixed; inset: 0; z-index: 0; pointer-events: none; }
        .bg-glow {
            position: fixed; inset: -30vh -30vw; z-index: 0; pointer-events: none;
            background: radial-gradient(closest-side at 20% 25%, rgba(0,240,255,0.14) 0%, transparent 68%),
                radial-gradient(closest-side at 78% 28%, rgba(122,92,255,0.16) 0%, transparent 70%);
        }
        .bg-grid {
            position: fixed; left: -20%; right: -20%; bottom: -8vh; height: 48vh; z-index: 0;
            pointer-events: none;
            background: linear-gradient(rgba(0,240,255,0.12) 1px, transparent 1px),
                linear-gradient(90deg, rgba(0,240,255,0.08) 1px, transparent 1px);
            background-size: 42px 42px;
            transform: perspective(520px) rotateX(62deg);
            transform-origin: center bottom;
            opacity: 0.5;
        }
        .auth-wrap { position: relative; z-index: 2; width: min(420px, 100%); }
        .card {
            background: rgba(10, 20, 50, 0.62);
            border: 1px solid var(--neon);
            border-radius: 18px;
            padding: 1.5rem;
            backdrop-filter: blur(14px);
            box-shadow: 0 0 32px rgba(0, 240, 255, 0.15), inset 0 0 24px rgba(122, 92, 255, 0.06);
            text-align: center;
        }
        .brand { font-family: 'Orbitron', sans-serif; font-size: 0.95rem; color: var(--gold); margin-bottom: 0.35rem; }
        h1 { margin: 0 0 8px; font-size: 1.25rem; font-family: 'Orbitron', sans-serif; color: var(--accent); text-shadow: 0 0 14px rgba(0,240,255,0.35); }
        p  { margin: 0 0 16px; color: var(--muted); font-size: 0.85rem; }
        label { display: block; font-size: 0.78rem; margin: 12px 0 6px; color: var(--muted); }
        input {
            width: 100%;
            padding: 11px 12px;
            border-radius: 12px;
            border: 1px solid rgba(34, 85, 255, 0.4);
            background: rgba(5, 10, 28, 0.85);
            color: var(--text);
            outline: none;
            text-align: center;
            font-family: inherit;
        }
        input:focus { border-color: var(--accent); box-shadow: 0 0 16px rgba(0,240,255,0.2); }
        button {
            border: 1px solid rgba(0, 240, 255, 0.4);
            padding: 12px 14px;
            border-radius: 14px;
            font-weight: 600;
            cursor: pointer;
            color: #fff;
            background: linear-gradient(135deg, #2563eb, #06b6d4);
            width: 100%;
            margin-top: 14px;
            font-family: inherit;
            box-shadow: 0 0 20px rgba(0,240,255,0.2);
        }
        .msg { margin-top: 12px; min-height: 20px; font-size: 0.85rem; }
        .err { color: var(--danger); }
        .ok { color: var(--success); }
    </style>
</head>
<body class="auth-page">
    <canvas id="starsBg"></canvas>
    <div class="bg-glow"></div>
    <div class="bg-grid"></div>
    <div class="auth-wrap">
    <div class="card">
        <div class="brand">꧁༺𝓒𝓲𝓬𝓪𝓭𝓪3301༻꧂</div>
        <h1>Авторизация</h1>
        <p>Введите пароль для доступа к панели.</p>
        <form method="POST" action="/auth/login" accept-charset="UTF-8">
        <label for="loginPwd">Пароль</label>
        <input id="loginPwd" name="password" type="password" placeholder="Введите пароль" autocomplete="current-password" required>
        <button id="loginBtn" type="submit">Войти</button>
        </form>
        <div id="msg" class="msg"></div>
    </div>
    </div>
    <script>
        (function(){var c=document.getElementById('starsBg');if(!c)return;var x=c.getContext('2d'),w=0,h=0,d=1,s=[],N=90;
        function rz(){d=Math.min(2,window.devicePixelRatio||1);w=window.innerWidth;h=window.innerHeight;
        c.width=Math.floor(w*d);c.height=Math.floor(h*d);c.style.width=w+'px';c.style.height=h+'px';
        x.setTransform(d,0,0,d,0,0);if(!s.length){for(var i=0;i<N;i++)s.push({x:Math.random()*w,y:Math.random()*h,r:.6+Math.random()*1.6,a:.2+Math.random()*.5,v:.1+Math.random()*.4});}}
        function tk(){x.clearRect(0,0,w,h);x.globalCompositeOperation='lighter';for(var i=0;i<s.length;i++){var t=s[i];
        t.y+=t.v;if(t.y>h+4){t.y=-4;t.x=Math.random()*w;}x.fillStyle='rgba(0,240,255,'+(t.a*.9)+')';
        x.beginPath();x.arc(t.x,t.y,t.r,0,6.28);x.fill();}requestAnimationFrame(tk);}
        window.addEventListener('resize',rz);rz();tk();})();
    </script>
    <script>
        (function() {
            var q = window.location.search || '';
            var msg = document.getElementById('msg');
            if (q.indexOf('err=1') >= 0 && msg) {
                msg.textContent = 'Неверный пароль';
                msg.className = 'msg err';
            }
        })();
    </script>
</body>
</html>
)AUTHHTML";

        void applyApIpFromSettings() {
            const access_point_settings_t& ap = settings::getAccessPointSettings();
            ip = IPAddress(ap.ip[0], ap.ip[1], ap.ip[2], ap.ip[3]);
        }

        IPAddress getPortalIp() {
            IPAddress apIp = WiFi.softAPIP();
            if ((uint32_t)apIp != 0) return apIp;
            return ip;
        }

        static String getPortalMdnsHost() {
            // Prefer current AP SSID (EEPROM settings) so the hostname changes
            // when the user changes SSID from the web UI.
            const access_point_settings_t& ap = settings::getAccessPointSettings();
            const char* src = ap.ssid;

            String out;
            out.reserve(32);

            for (int i = 0; src && src[i] && (int)out.length() < 32; i++) {
                const char c = src[i];
                const bool isAz = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
                const bool is09 = (c >= '0' && c <= '9');
                if (isAz) {
                    out += (char)tolower((unsigned char)c);
                } else if (is09) {
                    out += c;
                } else if (c == '-' || c == '_' || c == ' ') {
                    // mDNS host: keep it URL/Bonjour friendly
                    if (out.length() == 0 || out[out.length() - 1] == '-') continue;
                    out += '-';
                } else {
                    // skip other characters
                }
            }

            while (out.endsWith("-")) out.remove(out.length() - 1);
            if (out.length() < 3) {
                // If SSID contains non-ASCII (e.g. Cyrillic), sanitized hostname may be empty.
                // Use a deterministic ASCII fallback derived from AP MAC so the hostname changes.
                const wifi_settings_t& wf = settings::getWifiSettings();
                char macSuffix[7];
                snprintf(macSuffix, sizeof(macSuffix), "%02x%02x%02x",
                         (unsigned)wf.mac_ap[3], (unsigned)wf.mac_ap[4], (unsigned)wf.mac_ap[5]);
                out = String(F("esp-")) + String(macSuffix);
            }
            return out;
        }

        String getPortalBaseUrl() {
            // In repeater mode there is no captive DNS for local hostnames.
            // Use the SoftAP IP so browsers can always reach the UI.
            if (repeater_active || isRepeaterWorkmode()) {
                return String(F("http://")) + getPortalIp().toString();
            }
            // mDNS hostname is <host>.local on most clients (Android/iOS/Windows/macOS)
            return String(F("http://")) + getPortalMdnsHost() + String(F(".local"));
        }

        String getPortalUrl(const char* path) {
            String url = getPortalBaseUrl();
            if (path[0] != '/') url += '/';
            url += path;
            return url;
        }

        bool isCaptivePortalEnabled() {
            // В режиме ретранслятора — без captive DNS/редиректов (интернет через NAT)
            if (repeater_active) return false;
            if (!portal_pref_captive) return false;
            return mode == wifi_mode_t::ap;
        }

        bool isAuthEnabled() {
            // На созданной AP ретранслятора — без страницы авторизации
            if (repeater_active) return false;
            if (!portal_pref_auth) return false;
            return portal_auth_password.length() > 0;
        }

        bool isAuthorizedClient() {
            if (repeater_active) return true;
            if (!isAuthEnabled()) return true;
            if (portal_auth_ip == IPAddress(0, 0, 0, 0)) return false;
            return server.client().remoteIP() == portal_auth_ip;
        }

        void updateCaptiveDns() {
            if (dns_captive_active) {
                dns.stop();
                dns_captive_active = false;
            }

            if (isCaptivePortalEnabled() && (mode == wifi_mode_t::ap)) {
                dns.setErrorReplyCode(DNSReplyCode::NoError);
                dns.setTTL(0);
                dns.start(53, "*", getPortalIp());
                dns_captive_active = true;
                prntln("[WiFi] Captive DNS started");
            }
        }

        void sendConnectivitySuccess() {
            const String& uri = server.uri();

            if (uri == "/generate_204") {
                server.send(204, str(W_TXT), "");
            } else if (uri == "/connecttest.txt") {
                server.send(200, str(W_TXT), "Microsoft Connect Test");
            } else if (uri == "/hotspot-detect.html" || uri == "/fakeurl.html") {
                server.send(200, str(W_HTML), "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
            } else if (uri == "/wpad.dat") {
                server.send(404, str(W_TXT), "");
            } else {
                server.send(204, str(W_TXT), "");
            }
        }

        void loadAuthPassword() {
            if (!LittleFS.exists(AUTH_PASSWORD_PATH)) return;

            File f = LittleFS.open(AUTH_PASSWORD_PATH, "r");
            if (!f) return;

            String value = f.readStringUntil('\n');
            f.close();
            value.trim();

            if ((value.length() > 0) && (value.length() <= 64)) {
                portal_auth_password = value;
            }
        }

        void loadPortalPrefs() {
            portal_pref_captive = true;
            // Default web auth OFF unless explicitly enabled in /portal_prefs.json
            portal_pref_auth    = false;

            if (!LittleFS.exists(PORTAL_PREFS_PATH)) return;

            DynamicJsonBuffer jsonBuffer(256);
            JsonVariant parsed = parseJSONFile(String(PORTAL_PREFS_PATH), jsonBuffer);
            if (!parsed.success()) return;

            JsonObject& root = parsed.as<JsonObject>();
            portal_pref_captive = root["captive"] | true;
            portal_pref_auth    = root["auth"] | false;
        }

        bool savePortalPrefs() {
            DynamicJsonBuffer jsonBuffer(256);
            JsonObject& root = jsonBuffer.createObject();
            root["captive"] = portal_pref_captive;
            root["auth"]    = portal_pref_auth;
            String buf;
            root.printTo(buf);
            return writeFile(String(PORTAL_PREFS_PATH), buf);
        }

        bool saveAuthPassword(const String& value) {
            File f = LittleFS.open(AUTH_PASSWORD_PATH, "w");
            if (!f) return false;

            size_t written = f.print(value);
            f.close();
            return written == value.length();
        }

        void authorizeClient() {
            portal_auth_ip        = server.client().remoteIP();
            portal_auth_mac_valid = false;

            struct station_info* st     = wifi_softap_get_station_info();
            struct station_info* first  = st;
            struct station_info* matched = nullptr;

            while (st != NULL) {
                if (IPAddress(st->ip.addr) == portal_auth_ip) {
                    matched = st;
                    break;
                }
                st = STAILQ_NEXT(st, next);
            }

            if (!matched && first && (wifi_softap_get_station_num() == 1)) {
                matched = first;
            }

            if (matched) {
                memcpy(portal_auth_mac, matched->bssid, 6);
                portal_auth_mac_valid = true;
            }

            wifi_softap_free_station_info();
        }

        // Manual fallback only — never called automatically from bringUpSoftAP
        void startBackgroundApScan() {
            if (mode != wifi_mode_t::ap) return;
            scan.start(SCAN_MODE_APS, 0, SCAN_MODE_OFF, 0, true, ap_settings.channel);
        }

        bool isAuthPath(const String& uri) {
            return uri == "/auth" || uri == "/auth/login" || uri == "/update";
        }

        static bool portalAuthorizedForOta() {
            return isAuthorizedClient();
        }

        void sendAuthPage() {
            server.sendHeader(F("Cache-Control"), F("no-cache, no-store, must-revalidate"));
            server.sendHeader(F("Pragma"), F("no-cache"));
            // PROGMEM: send_P без size → strlen(RAM) → пустое тело → белый экран data:text/html,
            server.send_P(200, str(W_HTML).c_str(), AUTH_PAGE_HTML, strlen_P(AUTH_PAGE_HTML));
        }

        void sendCaptivePortalRedirect() {
            // For captive portal windows we prefer an absolute URL in AP mode,
            // so the browser shows a stable hostname instead of the probe domain.
            // In repeater mode, hostnames may not resolve (DNS is forwarded), so keep relative paths.
            String abs;
            const char* url = "/auth";
            if (!repeater_active && !isRepeaterWorkmode()) {
                abs = getPortalBaseUrl();
                abs += F("/auth");
                url = abs.c_str();
            }
            server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
            server.sendHeader("Pragma", "no-cache");
            server.sendHeader("Location", url, true);
            server.send(302, "text/plain", "");
        }

        void sendAuthRedirect() {
            sendCaptivePortalRedirect();
        }

        void sendIndexRedirect() {
            const char* path = (isRepeaterWorkmode() || repeater_active) ? "/repeater" : "/index";
            const char* url = path;
            server.sendHeader(F("Cache-Control"), F("no-cache, no-store, must-revalidate"));
            server.sendHeader(F("Pragma"), F("no-cache"));
            server.sendHeader(F("Location"), url, true);
            server.send(302, str(W_TXT), F("Redirect"));
        }

        static void sendRepeaterWorkmodeRedirect() {
            const char* url = "/repeater";
            server.sendHeader(F("Cache-Control"), F("no-cache, no-store, must-revalidate"));
            server.sendHeader(F("Pragma"), F("no-cache"));
            server.sendHeader(F("Location"), url, true);
            server.send(302, str(W_TXT), F("Redirect"));
        }

        bool ensureAuthorizedOrRedirect() {
            // Auth gating must work even when captive portal is disabled.
            if (!isAuthEnabled()) return true;
            if (isAuthorizedClient()) return true;

            // If captive is enabled, prefer HTML page (some captive browsers ignore 302).
            if (isCaptivePortalEnabled()) {
                sendAuthPage();
                return false;
            }

            // No captive portal: redirect normal browser to /auth
            server.sendHeader(F("Location"), F("/auth"), true);
            server.send(302, str(W_TXT), F("Redirect"));
            return false;
        }

        void handleCaptivePortalProbe() {
            // В режиме captive НИКОГДА не отвечаем 204 / «Success» (iOS/Android) —
            // иначе ОС пишет «Подключение выполнено» и сразу закрывает окно до index.html.
            if (!isCaptivePortalEnabled()) {
                // Captive выкл.: 204 на MIUI часто не убирает «Вход в сеть».
                // Тот же редирект, что при отключённой авторизации — закрывает окно captive.
                if (!isAuthEnabled() || isAuthorizedClient()) {
                    sendIndexRedirect();
                } else {
                    sendCaptivePortalRedirect();
                }
                return;
            }

            if (isAuthorizedClient()) {
                sendIndexRedirect();
                return;
            }

            // HTML сразу в окне «Войти в сеть» (MIUI/Android часто не грузят /auth после 302)
            if (isAuthEnabled()) {
                sendAuthPage();
                return;
            }

            sendCaptivePortalRedirect();
        }

        void handleCaptiveProbe(const char* path) {
            handleCaptivePortalProbe();
            DIAG_HTTP(path, 302);
        }

    void setPath(String path) {
        if (path.charAt(0) != '/') {
            path = '/' + path;
        }

        if (path.length() > 32) {
            debuglnF("ERROR: Path longer than 32 characters");
        } else {
            strncpy(ap_settings.path, path.c_str(), 32);
        }
    }

    void setSSID(String ssid) {
        if (ssid.length() > 32) {
            debuglnF("ERROR: SSID longer than 32 characters");
        } else {
            strncpy(ap_settings.ssid, ssid.c_str(), 32);
        }
    }

    void setPassword(String password) {
        if (password.length() > 64) {
            debuglnF("ERROR: Password longer than 64 characters");
        } else if (password.length() == 0) {
            ap_settings.password[0] = '\0';
        } else if (password.length() < 8) {
            debuglnF("ERROR: Password must be at least 8 characters long");
        } else {
            strncpy(ap_settings.password, password.c_str(), 64);
        }
    }

    void setChannel(uint8_t ch) {
        if ((ch < 1) || (ch > 14)) {
            debuglnF("ERROR: Channel must be withing the range of 1-14");
            ap_settings.channel = 1;
        } else {
            ap_settings.channel = ch;
        }
    }

    void setHidden(bool hidden) {
        ap_settings.hidden = hidden;
    }

    void setCaptivePortal(bool captivePortal) {
        ap_settings.captive_portal = captivePortal;
    }

    static void kickSoftApClients() {
        if (mode != wifi_mode_t::ap || repeater_active) return;

        WiFi.softAPdisconnect(true);
        yield();
        delay(80);

        if (ap_settings.password[0] == '\0') {
            WiFi.softAP(ap_settings.ssid, NULL, ap_settings.channel, ap_settings.hidden);
        } else {
            WiFi.softAP(ap_settings.ssid, ap_settings.password, ap_settings.channel, ap_settings.hidden);
        }
        yield();

        clearPortalAuth();
        ap_station_count_prev = 0;
        prntln("[WiFi] SoftAP bounced — clients must reconnect");
    }

    static void bringUpSoftAP(bool backgroundApScan, bool resetTracker) {
        (void)backgroundApScan;
        applyApIpFromSettings();

        // Keep softAP on the channel used for sniff / set channel
        if (ap_settings.channel < 1 || ap_settings.channel > 14) ap_settings.channel = wifi_channel;
        else if (wifi_channel >= 1 && wifi_channel <= 14) ap_settings.channel = wifi_channel;

        WiFi.softAPdisconnect(true);
        yield();

        wifi_set_opmode(STATIONAP_MODE);
        WiFi.softAPConfig(ip, ip, netmask);
        if (ap_settings.password[0] == '\0') {
            WiFi.softAP(ap_settings.ssid, NULL, ap_settings.channel, ap_settings.hidden);
        } else {
            WiFi.softAP(ap_settings.ssid, ap_settings.password, ap_settings.channel, ap_settings.hidden);
        }
        wifi_set_opmode(STATIONAP_MODE);
        yield();

        installPortalAuthStationHandlers();
        ap_station_count_prev = wifi_softap_get_station_num();

        mode = wifi_mode_t::ap;
        if (repeater_active) {
            setCaptivePortal(false);
            prntln("[Repeater] Captive portal disabled for repeater mode");
        } else {
            setCaptivePortal(portal_pref_captive);
        }
        updateCaptiveDns();

        {
            const String host = getPortalMdnsHost();
            MDNS.begin(host.c_str());
        }
        MDNS.addService("http", "tcp", 80);

        // Promiscuous + softAP + scanNetworks → WDT; not needed in repeater workmode
        if (isRepeaterWorkmode()) {
            wifi_promiscuous_enable(0);
            monitor_active = false;
            WiFi.persistent(false);
            WiFi.mode(WIFI_AP_STA);
            yield();
            WiFi.softAPConfig(ip, ip, netmask);
            if (ap_settings.password[0] == '\0') {
                WiFi.softAP(ap_settings.ssid, NULL, ap_settings.channel, ap_settings.hidden);
            } else {
                WiFi.softAP(ap_settings.ssid, ap_settings.password, ap_settings.channel, ap_settings.hidden);
            }
            yield();
        } else {
            enableMonitorMode();
        }

        if (resetTracker) clientTracker.begin();

        prntln(W_STARTED_AP);
        printStatus();
    }

    void handleFileList() {
        if (!server.hasArg("dir")) {
            server.send(500, str(W_TXT), str(W_BAD_ARGS));
            return;
        }

        String path = server.arg("dir");
        // debugF("handleFileList: ");
        // debugln(path);

        Dir dir = LittleFS.openDir(path);

        String output = String('{'); // {
        File   entry;
        bool   first = true;

        while (dir.next()) {
            entry = dir.openFile("r");

            if (first) first = false;
            else output += ',';                 // ,

            output += '[';                      // [
            output += '"' + entry.name() + '"'; // "filename"
            output += ']';                      // ]

            entry.close();
        }

        output += CLOSE_BRACKET;
        server.send(200, str(W_JSON).c_str(), output);
    }

    String getContentType(String filename) {
        if (server.hasArg("download")) return String(F("application/octet-stream"));
        else if (filename.endsWith(str(W_DOT_GZIP))) filename = filename.substring(0, filename.length() - 3);
        else if (filename.endsWith(str(W_DOT_HTM))) return str(W_HTML);
        else if (filename.endsWith(str(W_DOT_HTML))) return str(W_HTML);
        else if (filename.endsWith(str(W_DOT_CSS))) return str(W_CSS);
        else if (filename.endsWith(str(W_DOT_JS))) return str(W_JS);
        else if (filename.endsWith(str(W_DOT_PNG))) return str(W_PNG);
        else if (filename.endsWith(str(W_DOT_GIF))) return str(W_GIF);
        else if (filename.endsWith(str(W_DOT_JPG))) return str(W_JPG);
        else if (filename.endsWith(str(W_DOT_ICON))) return str(W_ICON);
        else if (filename.endsWith(str(W_DOT_XML))) return str(W_XML);
        else if (filename.endsWith(str(W_DOT_PDF))) return str(W_XPDF);
        else if (filename.endsWith(str(W_DOT_ZIP))) return str(W_XZIP);
        else if (filename.endsWith(str(W_DOT_JSON))) return str(W_JSON);
        return str(W_TXT);
    }

    bool handleFileRead(String path) {
        // prnt(W_AP_REQUEST);
        // prnt(path);

        if (path.charAt(0) != '/') path = '/' + path;

        if (path.endsWith(F("scan.json"))) {
            const String* cached = nullptr;

            if (scan.peekJsonCache(cached)) {
                DIAG_LOG(DIAG_DEBUG, "HTTP", "scan.json snapshot hit");
                server.sendHeader(F("Cache-Control"), F("no-cache"));
                server.send(200, str(W_JSON), *cached);
                return true;
            }

            DIAG_LOG(DIAG_DEBUG, "HTTP", "scan.json snapshot miss");
        }

        if (path.endsWith(F("names.json"))) {
            const String* cached = nullptr;

            if (names.peekJsonCache(cached)) {
                DIAG_LOG(DIAG_DEBUG, "HTTP", "names.json snapshot hit");
                server.sendHeader(F("Cache-Control"), F("no-cache"));
                server.send(200, str(W_JSON), *cached);
                return true;
            }

            DIAG_LOG(DIAG_DEBUG, "HTTP", "names.json snapshot miss");
        }

        // HTML ссылается на css/…, а в LittleFS файлы лежат в /web/ без подпапки css/
        if (path == F("/css/cicada_theme.css")) {
            path = String(ap_settings.path) + F("/cicada_theme.css");
        }

        if (path.charAt(path.length() - 1) == '/') path += String(F("index.html"));
        else if (path == F("/index") || path == F("/scan") || path == F("/radar") || path == F("/info") ||
                 path == F("/ssids") || path == F("/attack") || path == F("/settings") || path == F("/repeater")) {
            path += str(W_DOT_HTML);
        }

        String contentType = getContentType(path);

        if (!LittleFS.exists(path)) {
            if (LittleFS.exists(path + str(W_DOT_GZIP))) path += str(W_DOT_GZIP);
            else if (LittleFS.exists(String(ap_settings.path) + path)) path = String(ap_settings.path) + path;
            else if (LittleFS.exists(String(ap_settings.path) + path + str(W_DOT_GZIP))) path = String(ap_settings.path) + path + str(W_DOT_GZIP);
            else {
                // prntln(W_NOT_FOUND);
                return false;
            }
        }

        File file = LittleFS.open(path, "r");

        server.streamFile(file, contentType);
        file.close();
        // prnt(SPACE);
        // prntln(W_OK);

        return true;
    }

    void sendProgmem(const char* ptr, size_t size, const char* type) {
        server.sendHeader("Content-Encoding", "gzip");
        server.sendHeader("Cache-Control", "max-age=3600");
        server.send_P(200, str(type).c_str(), ptr, size);
    }

    static void sendRepeaterPage() {
#ifdef USE_PROGMEM_WEB_FILES
        if (!settings::getWebSettings().use_spiffs) {
            sendProgmem(repeaterhtml, sizeof(repeaterhtml), W_HTML);
            return;
        }
#endif
        if (!handleFileRead(F("/web/repeater.html"))) {
            server.send(404, str(W_TXT), str(W_FILE_NOT_FOUND));
        }
    }

    static void suspendWifiForRepeaterSta() {
        suspendMonitorMode();
        attack.stop();
        wifi_promiscuous_enable(0);
        monitor_active = false;
    }

    static void serviceLoopWhileBlocked() {
        currentTime = millis();
        scan.update();
        attack.update();
        clientTracker.tick();
        led::update();
        hsCaptureUpdate();
    }

    static void beginRepeaterTestAsync(const String& upSsid, const String& upPass) {
        if (repeater_async_phase != RepeaterAsyncPhase::Idle) return;

        repeater_async_up_ssid  = upSsid;
        repeater_async_up_pass  = upPass;
        repeater_async_test     = true;
        repeater_async_ok       = false;
        repeater_async_phase    = RepeaterAsyncPhase::Suspend;
        repeater_async_t0       = millis();
    }

    static void beginRepeaterBridgeAsync(const String& upSsid, const String& upPass, const String& apSsid,
                                         const String& apPass, bool rebootAfterFinish) {
        if (repeater_async_phase != RepeaterAsyncPhase::Idle) return;

        prntln("[Repeater] Upstream SSID: " + upSsid);
        prntln("[Repeater] AP SSID: " + apSsid);

        repeater_async_reboot_after_finish = rebootAfterFinish;
        repeater_async_up_ssid  = upSsid;
        repeater_async_up_pass  = upPass;
        repeater_async_ap_ssid  = apSsid;
        repeater_async_ap_pass  = apPass;
        repeater_async_test     = false;
        repeater_async_ok       = false;
        repeater_async_phase    = RepeaterAsyncPhase::Suspend;
        repeater_async_t0       = millis();
    }

    static void repeaterAsyncTick() {
        if (repeater_async_phase == RepeaterAsyncPhase::Idle) return;

        repeaterAsyncPollHttp();
        serviceLoopWhileBlocked();

        switch (repeater_async_phase) {
            case RepeaterAsyncPhase::Suspend:
                suspendWifiForRepeaterSta();

                if (repeater_async_test && !isApActive()) {
                    applyApIpFromSettings();

                    if (ap_settings.password[0] == '\0') {
                        WiFi.softAP(ap_settings.ssid, NULL, ap_settings.channel, ap_settings.hidden);
                    } else {
                        WiFi.softAP(ap_settings.ssid, ap_settings.password, ap_settings.channel, ap_settings.hidden);
                    }

                    WiFi.softAPConfig(ip, ip, netmask);
                }

                repeater_async_phase = RepeaterAsyncPhase::Delay;
                repeater_async_t0    = millis();
                break;

            case RepeaterAsyncPhase::Delay:
                if (millis() - repeater_async_t0 < (repeater_async_test ? 120u : 80u)) return;

                if (repeater_async_test) {
                    WiFi.persistent(false);
                    WiFi.mode(WIFI_AP_STA);
                    yield();
                    WiFi.disconnect(false);
                    yield();
                    repeater_async_phase = RepeaterAsyncPhase::TestBegin;
                    repeater_async_t0    = millis();
                } else {
                    repeater_async_phase = RepeaterAsyncPhase::BridgeModeOff;
                }

                break;

            case RepeaterAsyncPhase::TestBegin:
                if (millis() - repeater_async_t0 < 120) return;

                WiFi.begin(repeater_async_up_ssid.c_str(), repeater_async_up_pass.c_str());
                repeater_async_phase = RepeaterAsyncPhase::TestWait;
                repeater_async_t0    = millis();
                repeater_async_log   = repeater_async_t0;
                break;

            case RepeaterAsyncPhase::TestWait:
                if (WiFi.status() == WL_CONNECTED) {
                    repeater_async_ok = true;
                    prntln("[Repeater] Test OK · IP " + WiFi.localIP().toString() + " · ch " + String(WiFi.channel()));
                    repeaterAsyncReset();
                    return;
                }

                if (millis() - repeater_async_log > 4000) {
                    prntln("[Repeater] Test… status=" + String(WiFi.status()));
                    repeater_async_log = millis();
                }

                if (millis() - repeater_async_t0 > 25000) {
                    repeater_async_ok = false;
                    prntln("[Repeater] Test FAILED · status=" + String(WiFi.status()));
                    WiFi.disconnect(false);
                    yield();

                    if (!repeater_active && !isRepeaterWorkmode()) enableMonitorMode();

                    repeaterAsyncReset();
                }

                break;

            case RepeaterAsyncPhase::BridgeModeOff:
                suspendMonitorMode();
                attack.stop();
                WiFi.persistent(false);
                WiFi.mode(WIFI_OFF);
                yield();
                repeater_async_phase = RepeaterAsyncPhase::BridgeApCreate;
                repeater_async_t0    = millis();
                break;

            case RepeaterAsyncPhase::BridgeApCreate:
                if (millis() - repeater_async_t0 < 80) return;

                applyApIpFromSettings();
                WiFi.mode(WIFI_AP_STA);
                yield();

                {
                    const char* apPassC = (repeater_async_ap_pass.length() > 0) ? repeater_async_ap_pass.c_str() : nullptr;

                    prntln("[Repeater] Creating AP on channel " + String(ap_settings.channel));

                    if (!WiFi.softAP(repeater_async_ap_ssid.c_str(), apPassC, ap_settings.channel, ap_settings.hidden)) {
                        prntln("[Repeater] ERROR: Failed to create AP");
                        repeater_active = false;
                        repeaterAsyncReset();
                        return;
                    }
                }

                prntln("[Repeater] AP created successfully");
                WiFi.softAPConfig(ip, ip, netmask);
                yield();
                repeater_async_phase = RepeaterAsyncPhase::BridgeStaBegin;
                repeater_async_t0    = millis();
                break;

            case RepeaterAsyncPhase::BridgeStaBegin:
                if (millis() - repeater_async_t0 < 40) return;

                prntln("[Repeater] Connecting to upstream network...");
                WiFi.disconnect(false);
                yield();
                WiFi.begin(repeater_async_up_ssid.c_str(), repeater_async_up_pass.c_str());
                repeater_async_phase = RepeaterAsyncPhase::BridgeStaWait;
                repeater_async_t0    = millis();
                repeater_async_log   = repeater_async_t0;
                break;

            case RepeaterAsyncPhase::BridgeStaWait:
                if (WiFi.status() == WL_CONNECTED) {
                    prntln("[Repeater] Connected to upstream! Elapsed: " + String(millis() - repeater_async_t0) + "ms");

                    const uint8_t upCh = WiFi.channel();

                    if (upCh >= 1 && upCh <= 14) {
                        ap_settings.channel = upCh;
                        WiFi.softAPdisconnect(true);
                        yield();
                        repeater_async_phase = RepeaterAsyncPhase::BridgeApRetune;
                        repeater_async_t0    = millis();
                    } else {
                        repeater_async_phase = RepeaterAsyncPhase::BridgeFinish;
                    }

                    return;
                }

                if (millis() - repeater_async_log > 3000) {
                    prntln("[Repeater] Connecting... Status: " + String(WiFi.status()));
                    repeater_async_log = millis();
                }

                if (millis() - repeater_async_t0 > 25000) {
                    prntln("[Repeater] ERROR: Failed to connect to upstream (timeout after 25s)");
                    repeater_active = false;
                    repeaterAsyncReset();
                }

                break;

            case RepeaterAsyncPhase::BridgeApRetune:
                if (millis() - repeater_async_t0 < 100) return;

                {
                    const char* apPassC = (repeater_async_ap_pass.length() > 0) ? repeater_async_ap_pass.c_str() : nullptr;

                    prntln("[Repeater] Switching AP to channel " + String(ap_settings.channel));
                    WiFi.softAP(repeater_async_ap_ssid.c_str(), apPassC, ap_settings.channel, ap_settings.hidden);
                    WiFi.softAPConfig(ip, ip, netmask);
                    prntln("[Repeater] AP switched to channel " + String(ap_settings.channel));
                }

                repeater_async_phase = RepeaterAsyncPhase::BridgeFinish;
                break;

            case RepeaterAsyncPhase::BridgeFinish: {
                strncpy(ap_settings.ssid, repeater_async_ap_ssid.c_str(), 32);
                ap_settings.ssid[32] = '\0';

                if (repeater_async_ap_pass.length() >= 8) {
                    strncpy(ap_settings.password, repeater_async_ap_pass.c_str(), 64);
                    ap_settings.password[64] = '\0';
                } else {
                    ap_settings.password[0] = '\0';
                }

                repeater_up_ssid = repeater_async_up_ssid;
                repeater_ap_ssid = repeater_async_ap_ssid;
                repeater_active  = true;
                repeater_async_ok = true;

                mode = wifi_mode_t::ap;
                clearPortalAuth();

                if (dns_captive_active) {
                    dns.stop();
                    dns_captive_active = false;
                }

                setCaptivePortal(false);
                wifi_promiscuous_enable(0);
                monitor_active = false;

                prntln("[Repeater] Bridge ready, enabling NAPT...");
                enableRepeaterNapt();
                prntln("[Repeater] Bridge fully operational!");

                saveRepeaterConfig(repeater_async_up_ssid, repeater_async_up_pass, repeater_async_ap_ssid,
                                   repeater_async_ap_pass);
                repeaterAsyncReset();
                if (repeater_async_reboot_after_finish) {
                    prntln("[Repeater/API] Rebooting device...");
                    delay(150);
                    ESP.restart();
                }
                break;
            }

            default:
                repeaterAsyncReset();
                break;
        }
    }

    static bool testUpstreamConnection(const String& upSsid, const String& upPass) {
        if (repeater_async_phase != RepeaterAsyncPhase::Idle) return false;

        beginRepeaterTestAsync(upSsid, upPass);

        const uint32_t start = millis();

        while (repeater_async_phase != RepeaterAsyncPhase::Idle && millis() - start < 25000) {
            repeaterAsyncTick();
            delay(10);
            yield();
        }

        if (repeater_async_phase != RepeaterAsyncPhase::Idle) {
            repeater_async_ok = false;
            repeaterAsyncReset();
        }

        return repeater_async_ok;
    }

    static void cancelUpstreamTest() {
        if (repeater_async_phase != RepeaterAsyncPhase::Idle && !repeater_async_test) return;

        WiFi.disconnect(false);
        yield();
        repeaterAsyncReset();

        if (!repeater_active && !isRepeaterWorkmode()) enableMonitorMode();
    }

    // ===== PUBLIC ====== //
    void begin() {
        // Set settings
        setPath("/web");
        setSSID(settings::getAccessPointSettings().ssid);
        setPassword(settings::getAccessPointSettings().password);
        setChannel(settings::getWifiSettings().channel);
        setHidden(settings::getAccessPointSettings().hidden);
        
        if (isRepeaterWorkmode()) {
            prntln("[WiFi] Repeater workmode — bridge auto-starts if /repeater.json exists");
        }

        // copy web files to SPIFFS
        if (settings::getWebSettings().use_spiffs) {
            copyWebFiles(false);
        }

        loadAuthPassword();
        loadPortalPrefs();
        setCaptivePortal(portal_pref_captive);

        // Set mode
        mode = wifi_mode_t::off;
        WiFi.mode(WIFI_OFF);
        wifi_set_opmode(STATION_MODE);

        // Set mac address
        wifi_set_macaddr(STATION_IF, (uint8_t*)settings::getWifiSettings().mac_st);
        wifi_set_macaddr(SOFTAP_IF, (uint8_t*)settings::getWifiSettings().mac_ap);
    }

    String getMode() {
        switch (mode) {
            case wifi_mode_t::off:
                return "OFF";
            case wifi_mode_t::ap:
                return "AP";
            case wifi_mode_t::st:
                return "ST";
            default:
                return String();
        }
    }

    void printStatus() {
        prnt(String(F("[WiFi] Path: '")));
        prnt(ap_settings.path);
        prnt(String(F("', Mode: '")));
        prnt(getMode());
        prnt(String(F("', SSID: '")));
        prnt(ap_settings.ssid);
        prnt(String(F("', password: '")));
        prnt(ap_settings.password);
        prnt(String(F("', channel: '")));
        prnt(ap_settings.channel);
        prnt(String(F("', hidden: ")));
        prnt(b2s(ap_settings.hidden));
        prnt(String(F(", captive-portal: ")));
        prntln(b2s(ap_settings.captive_portal));
    }

    void startNewAP(String path, String ssid, String password, uint8_t ch, bool hidden) {
        setPath(path);
        setSSID(ssid);
        setPassword(password);
        setChannel(ch);
        setHidden(hidden);
        setCaptivePortal(portal_pref_captive);

        startAP();
    }

    /*
        void startAP(String path) {
            setPath(path):

            startAP();
        }
     */
    bool isApActive() {
        return mode == wifi_mode_t::ap;
    }

    bool isRepeaterWorkmode() {
        return settings::getAllSettings().workmode == 1;
    }

    void startAP(bool backgroundApScan) {
        clearPortalAuth();
        ap_station_count_prev = 0;
        bringUpSoftAP(backgroundApScan, true);

        server.on("/list", HTTP_GET, handleFileList); // list directory

        #ifdef USE_PROGMEM_WEB_FILES
        // ================================================================
        // paste here the output of the webConverter.py
        if (!settings::getWebSettings().use_spiffs) {
            server.on("/", HTTP_GET, []() {
                if (!isAuthorizedClient() && isAuthEnabled()) {
                    sendAuthPage();
                    return;
                }
                if (isRepeaterWorkmode()) {
                    sendRepeaterWorkmodeRedirect();
                    return;
                }
                sendProgmem(indexhtml, sizeof(indexhtml), W_HTML);
            });
            server.on("/index.html", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                if (isRepeaterWorkmode()) {
                    sendRepeaterWorkmodeRedirect();
                    return;
                }
                sendProgmem(indexhtml, sizeof(indexhtml), W_HTML);
            });
            server.on("/index", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                if (isRepeaterWorkmode()) {
                    sendRepeaterWorkmodeRedirect();
                    return;
                }
                sendProgmem(indexhtml, sizeof(indexhtml), W_HTML);
            });
            server.on("/scan.html", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                if (settings::getAllSettings().workmode == 1) { sendRepeaterPage(); return; }
                sendProgmem(scanhtml, sizeof(scanhtml), W_HTML);
            });
            server.on("/scan", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                if (settings::getAllSettings().workmode == 1) { sendRepeaterPage(); return; }
                sendProgmem(scanhtml, sizeof(scanhtml), W_HTML);
            });
            server.on("/radar.html", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                if (settings::getAllSettings().workmode == 1) { sendRepeaterPage(); return; }
                sendProgmem(radarhtml, sizeof(radarhtml), W_HTML);
            });
            server.on("/radar", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                if (settings::getAllSettings().workmode == 1) { sendRepeaterPage(); return; }
                sendProgmem(radarhtml, sizeof(radarhtml), W_HTML);
            });
            server.on("/info.html", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(infohtml, sizeof(infohtml), W_HTML);
            });
            server.on("/info", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(infohtml, sizeof(infohtml), W_HTML);
            });
            server.on("/ssids.html", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                if (settings::getAllSettings().workmode == 1) { sendRepeaterPage(); return; }
                sendProgmem(ssidshtml, sizeof(ssidshtml), W_HTML);
            });
            server.on("/ssids", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                if (settings::getAllSettings().workmode == 1) { sendRepeaterPage(); return; }
                sendProgmem(ssidshtml, sizeof(ssidshtml), W_HTML);
            });
            server.on("/attack.html", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                if (settings::getAllSettings().workmode == 1) { sendRepeaterPage(); return; }
                sendProgmem(attackhtml, sizeof(attackhtml), W_HTML);
            });
            server.on("/attack", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                if (settings::getAllSettings().workmode == 1) { sendRepeaterPage(); return; }
                sendProgmem(attackhtml, sizeof(attackhtml), W_HTML);
            });
            server.on("/settings.html", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(settingshtml, sizeof(settingshtml), W_HTML);
            });
            server.on("/settings", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(settingshtml, sizeof(settingshtml), W_HTML);
            });
            server.on("/style.css", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(stylecss, sizeof(stylecss), W_CSS);
            });
            server.on("/js/ssids.js", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(ssidsjs, sizeof(ssidsjs), W_JS);
            });
            server.on("/js/site.js", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(sitejs, sizeof(sitejs), W_JS);
            });
            server.on("/js/attack.js", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(attackjs, sizeof(attackjs), W_JS);
            });
            server.on("/js/scan.js", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(scanjs, sizeof(scanjs), W_JS);
            });
            server.on("/js/radar.js", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(radarjs, sizeof(radarjs), W_JS);
            });
            server.on("/js/settings.js", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(settingsjs, sizeof(settingsjs), W_JS);
            });
            server.on("/lang/hu.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(hulang, sizeof(hulang), W_JSON);
            });
            server.on("/lang/ja.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(jalang, sizeof(jalang), W_JSON);
            });
            server.on("/lang/nl.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(nllang, sizeof(nllang), W_JSON);
            });
            server.on("/lang/fi.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(filang, sizeof(filang), W_JSON);
            });
            server.on("/lang/cn.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(cnlang, sizeof(cnlang), W_JSON);
            });
            server.on("/lang/ru.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(rulang, sizeof(rulang), W_JSON);
            });
            server.on("/lang/pl.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(pllang, sizeof(pllang), W_JSON);
            });
            server.on("/lang/uk.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(uklang, sizeof(uklang), W_JSON);
            });
            server.on("/lang/de.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(delang, sizeof(delang), W_JSON);
            });
            server.on("/lang/it.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(itlang, sizeof(itlang), W_JSON);
            });
            server.on("/lang/en.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(enlang, sizeof(enlang), W_JSON);
            });
            server.on("/lang/fr.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(frlang, sizeof(frlang), W_JSON);
            });
            server.on("/lang/in.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(inlang, sizeof(inlang), W_JSON);
            });
            server.on("/lang/ko.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(kolang, sizeof(kolang), W_JSON);
            });
            server.on("/lang/ro.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(rolang, sizeof(rolang), W_JSON);
            });
            server.on("/lang/da.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(dalang, sizeof(dalang), W_JSON);
            });
            server.on("/lang/ptbr.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(ptbrlang, sizeof(ptbrlang), W_JSON);
            });
            server.on("/lang/cs.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(cslang, sizeof(cslang), W_JSON);
            });
            server.on("/lang/tlh.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(tlhlang, sizeof(tlhlang), W_JSON);
            });
            server.on("/lang/es.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(eslang, sizeof(eslang), W_JSON);
            });
            server.on("/lang/th.lang", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                sendProgmem(thlang, sizeof(thlang), W_JSON);
            });
        }
        server.on("/lang/default.lang", HTTP_GET, []() {
            if (!ensureAuthorizedOrRedirect()) return;
            if (!settings::getWebSettings().use_spiffs) {
                if (String(settings::getWebSettings().lang) == "hu") sendProgmem(hulang, sizeof(hulang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "ja") sendProgmem(jalang, sizeof(jalang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "nl") sendProgmem(nllang, sizeof(nllang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "fi") sendProgmem(filang, sizeof(filang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "cn") sendProgmem(cnlang, sizeof(cnlang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "ru") sendProgmem(rulang, sizeof(rulang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "pl") sendProgmem(pllang, sizeof(pllang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "uk") sendProgmem(uklang, sizeof(uklang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "de") sendProgmem(delang, sizeof(delang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "it") sendProgmem(itlang, sizeof(itlang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "en") sendProgmem(enlang, sizeof(enlang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "fr") sendProgmem(frlang, sizeof(frlang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "in") sendProgmem(inlang, sizeof(inlang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "ko") sendProgmem(kolang, sizeof(kolang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "ro") sendProgmem(rolang, sizeof(rolang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "da") sendProgmem(dalang, sizeof(dalang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "ptbr") sendProgmem(ptbrlang, sizeof(ptbrlang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "cs") sendProgmem(cslang, sizeof(cslang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "tlh") sendProgmem(tlhlang, sizeof(tlhlang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "es") sendProgmem(eslang, sizeof(eslang), W_JSON);
                else if (String(settings::getWebSettings().lang) == "th") sendProgmem(thlang, sizeof(thlang), W_JSON);

                else handleFileRead("/web/lang/"+String(settings::getWebSettings().lang)+".lang");
            } else {
                handleFileRead("/web/lang/"+String(settings::getWebSettings().lang)+".lang");
            }
        });
        // ================================================================
        #endif /* ifdef USE_PROGMEM_WEB_FILES */

#if defined(USE_PROGMEM_WEB_FILES)
        if (settings::getWebSettings().use_spiffs) {
            server.on("/", HTTP_GET, []() {
                if (!isAuthorizedClient() && isCaptivePortalEnabled() && isAuthEnabled()) {
                    sendAuthPage();
                    return;
                }
                if (isRepeaterWorkmode()) {
                    sendRepeaterWorkmodeRedirect();
                    return;
                }
                if (!handleFileRead(F("/index.html"))) {
                    server.send(404, str(W_TXT), str(W_FILE_NOT_FOUND));
                }
            });
            server.on("/index.html", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                if (isRepeaterWorkmode()) {
                    sendRepeaterWorkmodeRedirect();
                    return;
                }
                if (!handleFileRead(F("/index.html"))) {
                    server.send(404, str(W_TXT), str(W_FILE_NOT_FOUND));
                }
            });
            server.on("/index", HTTP_GET, []() {
                if (!ensureAuthorizedOrRedirect()) return;
                if (isRepeaterWorkmode()) {
                    sendRepeaterWorkmodeRedirect();
                    return;
                }
                if (!handleFileRead(F("/index"))) {
                    server.send(404, str(W_TXT), str(W_FILE_NOT_FOUND));
                }
            });
        }
#endif

        // Тема: без проверки auth (статика), работает и при use_spiffs=true
        server.on("/css/cicada_theme.css", HTTP_GET, []() {
#if defined(USE_PROGMEM_WEB_FILES)
            if (!settings::getWebSettings().use_spiffs) {
                sendProgmem(cicada_themecss, sizeof(cicada_themecss), W_CSS);
                return;
            }
#endif
            if (!handleFileRead(String(ap_settings.path) + F("/cicada_theme.css"))) {
                server.send(404, str(W_TXT), str(W_FILE_NOT_FOUND));
            }
        });
        server.on("/cicada_theme.css", HTTP_GET, []() {
#if defined(USE_PROGMEM_WEB_FILES)
            if (!settings::getWebSettings().use_spiffs) {
                sendProgmem(cicada_themecss, sizeof(cicada_themecss), W_CSS);
                return;
            }
#endif
            if (!handleFileRead(String(ap_settings.path) + F("/cicada_theme.css"))) {
                server.send(404, str(W_TXT), str(W_FILE_NOT_FOUND));
            }
        });
        server.on("/js/theme_bg.js", HTTP_GET, []() {
#if defined(USE_PROGMEM_WEB_FILES)
            if (!settings::getWebSettings().use_spiffs) {
                sendProgmem(theme_bgjs, sizeof(theme_bgjs), W_JS);
                return;
            }
#endif
            if (!handleFileRead(String(ap_settings.path) + F("/js/theme_bg.js"))) {
                server.send(404, str(W_TXT), str(W_FILE_NOT_FOUND));
            }
        });

        server.on("/repeater.html", HTTP_GET, []() {
            if (!ensureAuthorizedOrRedirect()) return;
            sendRepeaterPage();
        });
        server.on("/repeater", HTTP_GET, []() {
            if (!ensureAuthorizedOrRedirect()) return;
            sendRepeaterPage();
        });

#ifdef USE_PROGMEM_WEB_FILES
        server.on("/js/repeater.js", HTTP_GET, []() {
            if (!ensureAuthorizedOrRedirect()) return;
            if (!settings::getWebSettings().use_spiffs) {
                sendProgmem(repeaterjs, sizeof(repeaterjs), W_JS);
                return;
            }
            if (!handleFileRead(String(ap_settings.path) + F("/js/repeater.js"))) {
                server.send(404, str(W_TXT), str(W_FILE_NOT_FOUND));
            }
        });
#endif

        server.on("/repeater/status.json", HTTP_GET, []() {
            if (!ensureAuthorizedOrRedirect()) return;
            String out;
            out.reserve(420);
            out += '{';
            out += "\"repeaterActive\":";
            out += repeater_active ? "true" : "false";
            out += ",\"staConnected\":";
            out += (WiFi.status() == WL_CONNECTED) ? "true" : "false";
            out += ",\"staSsid\":\"";
            out += WiFi.SSID();
            out += "\",\"staIp\":\"";
            out += WiFi.localIP().toString();
            out += "\",\"staGw\":\"";
            out += WiFi.gatewayIP().toString();
            out += "\",\"staMask\":\"";
            out += WiFi.subnetMask().toString();
            out += "\",\"staDns0\":\"";
            out += WiFi.dnsIP(0).toString();
            out += "\",\"staChannel\":";
            out += String(WiFi.channel());
            out += ",\"apActive\":";
            out += isApActive() ? "true" : "false";
            out += ",\"apSsid\":\"";
            out += WiFi.softAPSSID();
            out += "\",\"apIp\":\"";
            out += WiFi.softAPIP().toString();
            out += "\",\"naptEnabled\":";
            out += (repeater_napt_enabled && (WiFi.status() == WL_CONNECTED)) ? "true" : "false";
            out += "}";
            server.send(200, W_JSON, out);
            // Periodic diagnostic logging
            static uint32_t lastDiag = 0;
            if (millis() - lastDiag > 10000) {
                lastDiag = millis();
                if (repeater_active) {
                    prntln("[Repeater] Status: Active | STA: " + String(WiFi.SSID()) + 
                           " (IP:" + WiFi.localIP().toString() + ", Ch:" + String(WiFi.channel()) + 
                           ") | AP:" + String(WiFi.softAPSSID()) + " | NAPT:" + 
                           (repeater_napt_enabled ? "ON" : "OFF"));
                }
            }
        });

        server.on("/repeater/scan", HTTP_GET, []() {
            if (!ensureAuthorizedOrRedirect()) return;
            if (!isRepeaterWorkmode()) {
                server.send(403, str(W_TXT), "workmode!=repeater");
                return;
            }
            if (scan.isScanning()) scan.stop();
            suspendMonitorMode();
            attack.stop();
            wifi_promiscuous_enable(0);
            monitor_active = false;
            WiFi.persistent(false);
            if (WiFi.getMode() != WIFI_AP_STA) {
                WiFi.mode(WIFI_AP_STA);
                yield();
            }
            // STA left connected after test-connect conflicts with scanNetworks → crash
            if (WiFi.status() == WL_CONNECTED) {
                WiFi.disconnect(false);
                yield();
                delay(150);
            }
            scan.start(SCAN_MODE_APS, 0, SCAN_MODE_OFF, 0, true, ap_settings.channel);
            server.send(200, str(W_TXT), "OK");
        });

        server.on("/repeater/test-connect", HTTP_GET, []() {
            if (!ensureAuthorizedOrRedirect()) return;
            if (settings::getAllSettings().workmode != 1) {
                server.send(403, str(W_TXT), "workmode!=repeater");
                return;
            }

            String upSsid = server.arg("upSsid");
            String upPass = server.arg("upPass");
            upSsid.trim();
            upPass.trim();

            if (upSsid.length() == 0) {
                server.send(400, str(W_TXT), "missing ssid");
                return;
            }

            const bool ok = testUpstreamConnection(upSsid, upPass);

            String out;
            out.reserve(180);
            out += '{';
            out += "\"ok\":";
            out += ok ? "true" : "false";
            out += ",\"staConnected\":";
            out += ok ? "true" : "false";
            out += ",\"staSsid\":\"";
            out += WiFi.SSID();
            out += "\",\"staIp\":\"";
            out += WiFi.localIP().toString();
            out += "\",\"staChannel\":";
            out += String(WiFi.channel());
            out += '}';
            server.send(200, W_JSON, out);
            DIAG_HTTP("/repeater/test-connect", ok ? 200 : 500);
        });

        server.on("/repeater/cancel-upstream", HTTP_GET, []() {
            if (!ensureAuthorizedOrRedirect()) return;
            cancelUpstreamTest();
            server.send(200, str(W_TXT), "OK");
        });

        server.on("/repeater/connect", HTTP_GET, []() {
            if (!ensureAuthorizedOrRedirect()) return;
            prntln("[Repeater/API] /repeater/connect request received");
            if (settings::getAllSettings().workmode != 1) {
                prntln("[Repeater/API] ERROR: Device not in repeater workmode");
                server.send(403, str(W_TXT), "workmode!=repeater");
                return;
            }

            String upSsid = server.arg("upSsid");
            String upPass = server.arg("upPass");
            String apSsid  = server.arg("apSsid");
            String apPass  = server.arg("apPass");

            upSsid.trim();
            upPass.trim();
            apSsid.trim();
            apPass.trim();

            prntln("[Repeater/API] Parameters: upSsid='" + upSsid + "', apSsid='" + apSsid + "'");

            if (upSsid.length() == 0 || apSsid.length() == 0) {
                prntln("[Repeater/API] ERROR: Missing SSID parameters");
                server.send(400, str(W_TXT), "missing ssid");
                return;
            }

            if (apPass.length() > 0 && apPass.length() < 8) {
                prntln("[Repeater/API] ERROR: AP password too short (< 8 chars)");
                server.send(400, str(W_TXT), "ap password < 8");
                return;
            }

            // Immediately respond so the browser page doesn't "hang" when Wi-Fi reconfigures.
            // The AP may disappear/change channel during setup, which otherwise aborts the request.
            {
                String ack;
                ack.reserve(180);
                ack += '{';
                ack += "\"accepted\":true";
                ack += ",\"upSsid\":\"";
                ack += upSsid;
                ack += "\",\"apSsid\":\"";
                ack += apSsid;
                ack += "\"}";
                server.send(200, W_JSON, ack);
                // Close client connection early (prevents long-polling on phones)
                server.client().stop();
                delay(5);
            }

            prntln("[Repeater/API] Starting bridge setup (async)...");
            // Preserve existing API behavior: reboot once after successful connect request.
            beginRepeaterBridgeAsync(upSsid, upPass, apSsid, apPass, true);
        });

        server.on("/repeater/disconnect", HTTP_GET, []() {
            if (!ensureAuthorizedOrRedirect()) return;
            prntln("[Repeater/API] Disconnect request - shutting down repeater");
            disableRepeaterNapt();
            repeater_active = false;
            repeater_up_ssid = "";
            repeater_ap_ssid = "";
            clearRepeaterConfig();
            repeater_boot_tried = false;
            WiFi.disconnect(false);
            setSSID(settings::getAccessPointSettings().ssid);
            setPassword(settings::getAccessPointSettings().password);
            setChannel(settings::getWifiSettings().channel);
            clearPortalAuth();
            ap_station_count_prev = 0;
            bringUpSoftAP(false, false);
            prntln("[Repeater/API] Repeater disconnected, reverting to normal AP mode");
            server.send(200, str(W_TXT), "OK");
        });

        server.on("/run", HTTP_GET, []() {
            if (!ensureAuthorizedOrRedirect()) return;
            server.send(200, str(W_TXT), str(W_OK).c_str());
            String input = server.arg("cmd");
            cli.exec(input);
        });

        server.on("/attack.json", HTTP_GET, []() {
            if (!ensureAuthorizedOrRedirect()) return;
            server.send(200, str(W_JSON), attack.getStatusJSON());
        });

        server.on("/auth/password", HTTP_GET, []() {
            if (!server.hasArg("value")) {
                server.send(200, str(W_TXT), portal_auth_password);
                return;
            }

            String value = server.arg("value");
            value.trim();

            if ((value.length() < 1) || (value.length() > 64)) {
                server.send(400, str(W_TXT), "Password length must be 1..64");
                return;
            }

            portal_auth_password = value;

            if (!saveAuthPassword(portal_auth_password)) {
                server.send(500, str(W_TXT), "Failed to save password");
                return;
            }

            server.send(200, str(W_TXT), str(W_OK));
        });

        // Portal prefs: captive portal and auth enable/disable
        server.on("/portal/prefs.json", HTTP_GET, []() {
            if (!ensureAuthorizedOrRedirect()) return;
            String out;
            out.reserve(80);
            out += '{';
            out += "\"captive\":";
            out += portal_pref_captive ? "true" : "false";
            out += ",\"auth\":";
            out += portal_pref_auth ? "true" : "false";
            out += '}';
            server.send(200, W_JSON, out);
        });

        server.on("/portal/prefs", HTTP_GET, []() {
            if (!ensureAuthorizedOrRedirect()) return;
            auto parseBoolArg = [](String v, bool defVal) -> bool {
                v.trim();
                v.toLowerCase();
                if (v.length() == 0) return defVal;
                if (v == "1" || v == "true" || v == "on" || v == "yes") return true;
                if (v == "0" || v == "false" || v == "off" || v == "no") return false;
                return defVal;
            };

            const bool captiveToggled = server.hasArg("captive");
            const bool authToggled    = server.hasArg("auth");
            const bool doBounce       = server.hasArg("bounce")
                && parseBoolArg(server.arg("bounce"), false);

            if (captiveToggled) portal_pref_captive = parseBoolArg(server.arg("captive"), portal_pref_captive);
            if (authToggled) portal_pref_auth = parseBoolArg(server.arg("auth"), portal_pref_auth);

            if (!savePortalPrefs()) {
                server.send(500, str(W_TXT), "Failed to save portal prefs");
                return;
            }

            if (captiveToggled) {
                updateCaptiveDns();
            }
            if (captiveToggled || authToggled) {
                clearPortalAuth();
            }
            // Разрыв SoftAP только по явному bounce=1 (кнопка «Сохранить» в веб-настройках).
            if (doBounce && portal_pref_captive) {
                kickSoftApClients();
            }
            server.send(200, str(W_TXT), "OK");
        });

        server.on("/auth", HTTP_GET, []() {
            sendAuthPage();
            DIAG_HTTP("/auth", 200);
        });

        server.on("/auth/login", HTTP_POST, []() {
            String password = server.arg("password");

            if (portal_auth_password.length() == 0) {
                server.send(400, str(W_TXT), "Password is not set. Create one first.");
                DIAG_HTTP("/auth/login", 400);
                return;
            }

            if (password == portal_auth_password) {
                authorizeClient();
                // 302 — браузер/captive сразу открывает index.html; HTML — запасной вариант
                sendIndexRedirect();
                DIAG_HTTP("/auth/login", 302);
            } else {
                server.sendHeader(F("Location"), F("/auth?err=1"), true);
                server.send(302, str(W_TXT), F("Unauthorized"));
                DIAG_HTTP("/auth/login", 302);
            }
        });

        // Captive portal connectivity checks (Android / iOS / Windows)
        server.on("/generate_204", HTTP_GET, []() {
            handleCaptiveProbe("/generate_204");
        });
        server.on("/hotspot-detect.html", HTTP_GET, []() {
            handleCaptiveProbe("/hotspot-detect.html");
        });
        server.on("/library/test/success.html", HTTP_GET, []() {
            handleCaptiveProbe("/library/test/success.html");
        });
        server.on("/connecttest.txt", HTTP_GET, []() {
            handleCaptiveProbe("/connecttest.txt");
        });
        server.on("/fwlink", HTTP_GET, []() {
            handleCaptiveProbe("/fwlink");
        });
        server.on("/ncsi.txt", HTTP_GET, []() {
            handleCaptiveProbe("/ncsi.txt");
        });
        server.on("/redirect", HTTP_GET, []() {
            handleCaptiveProbe("/redirect");
        });
        server.on("/gen_204", HTTP_GET, []() {
            handleCaptiveProbe("/gen_204");
        });
        server.on("/fakeurl.html", HTTP_GET, []() {
            handleCaptiveProbe("/fakeurl.html");
        });
        server.on("/wpad.dat", HTTP_GET, []() {
            handleCaptiveProbe("/wpad.dat");
        });
        server.on("/success.txt", HTTP_GET, []() {
            handleCaptiveProbe("/success.txt");
        });
        server.on("/check_network_status.txt", HTTP_GET, []() {
            handleCaptiveProbe("/check_network_status.txt");
        });
        server.on("/mobile/status.php", HTTP_GET, []() {
            handleCaptiveProbe("/mobile/status.php");
        });
        server.on("/canonical.html", HTTP_GET, []() {
            handleCaptiveProbe("/canonical.html");
        });

        firmwareUpdateRegister(server, portalAuthorizedForOta);

        // called when the url is not defined here
        // use it to load content from SPIFFS
        server.onNotFound([]() {
            if (!isAuthorizedClient() && isAuthEnabled() && !isAuthPath(server.uri())) {
                sendAuthPage();
                DIAG_HTTP(server.uri().c_str(), 200);
                return;
            }

            if (!handleFileRead(server.uri())) {
                if (isCaptivePortalEnabled()) sendProgmem(indexhtml, sizeof(indexhtml), W_HTML);
                else server.send(404, str(W_TXT), str(W_FILE_NOT_FOUND));
            }
        });

        server.begin();
    }

    void syncCaptivePortal() {
        if (!repeater_active) setCaptivePortal(portal_pref_captive);
        updateCaptiveDns();
    }

    void stopAP() {
        if (mode == wifi_mode_t::ap && !station_sniff_mode) {
            pauseApForSniff(wifi_channel);
        }
    }

    void resumeAP() {
        if (station_sniff_mode) {
            restoreApAfterSniff();
            return;
        }

        if (mode == wifi_mode_t::ap) {
            enableMonitorMode();
            return;
        }

        clearPortalAuth();
        ap_station_count_prev = 0;
        bringUpSoftAP(false, false);
    }

    void enableMonitorMode() {
        if (repeater_active || isRepeaterWorkmode()) return;

        if (station_sniff_mode) {
            enablePromiscuousRx(false);
            return;
        }

        if (!isApActive()) return;

        promisc_sniff_path = 0;
        enablePromiscuousRx(true);
    }

    void suspendMonitorMode() {
        wifi_promiscuous_enable(0);
    }

    void pauseApForSniff(uint8_t ch) {
        if (station_sniff_mode) return;

        station_sniff_mode  = true;
        promisc_isr_count   = 0;
        promisc_sniff_path  = 1;
        promisc_head        = 0;
        promisc_tail        = 0;
        promisc_drops       = 0;

        if (dns_captive_active) {
            dns.stop();
            dns_captive_active = false;
        }

        // Same sequence as upstream esp8266_deauther wifi::stopAP (required for promisc RX)
        wifi_promiscuous_enable(0);
        WiFi.persistent(false);
        WiFi.softAPdisconnect(true);
        WiFi.disconnect(true);
        wifi_station_disconnect();
        wifi_set_opmode(STATION_MODE);
        mode = wifi_mode_t::st;
        yield();

        installPromiscCallback();
        ::setWifiChannel(ch, true);
        yield();

        wifi_promiscuous_enable(1);
        monitor_active = true;

        DIAG_LOGF(DIAG_INFO, "WIFI", "STA sniff ch=%u (softAP off, mode=ST)", (unsigned)ch);
    }

    void restoreApAfterSniff() {
        if (!station_sniff_mode) return;

        station_sniff_mode = false;
        promisc_sniff_path = 0;

        wifi_promiscuous_enable(0);
        monitor_active = false;
        yield();

        clearPortalAuth();
        ap_station_count_prev = 0;
        bringUpSoftAP(false, false);

        DIAG_LOG(DIAG_INFO, "WIFI", "STA sniff done, softAP restored");
    }

    void enterStationSniffMode(uint8_t ch) {
        pauseApForSniff(ch);
    }

    void leaveStationSniffMode() {
        restoreApAfterSniff();
    }

    bool isStationSniffMode() {
        return station_sniff_mode;
    }

    void setMonitorChannel(uint8_t ch) {
        if (repeater_active || isRepeaterWorkmode()) {
            ::setWifiChannel(ch, true);
            return;
        }

        if (station_sniff_mode) {
            suspendMonitorMode();
            ::setWifiChannel(ch, true);
            enableMonitorMode();
            return;
        }

        bool reenable = monitor_active && isApActive();

        if (reenable) wifi_promiscuous_enable(0);

        ::setWifiChannel(ch, true);

        if (reenable) wifi_promiscuous_enable(1);
    }

    bool isMonitorActive() {
        return monitor_active;
    }

    void tickClients() {
        DIAG_PHASE(DIAG_P2, "TRACKER", "tick");
        clientTracker.tick();
    }

    void onPromiscuousRx(uint8_t* buf, uint16_t len) {
        bool sniffPath = promisc_sniff_path || scan.isSniffing() || hsCaptureActive();
        promiscEnqueue(buf, len, sniffPath);
    }

    uint32_t promiscIsrCount() {
        return promisc_isr_count;
    }

    uint16_t promiscQueueDrops() {
        return promisc_drops;
    }

    uint8_t promiscRingDepth() {
        return promiscRingDepthInternal();
    }

    uint8_t promiscRingCapacity() {
        return PROMISC_RING_SLOTS;
    }

    void update() {
        repeaterAsyncTick();

        if (mode == wifi_mode_t::off) return;

        if (isRepeaterWorkmode() && !repeater_boot_tried && millis() > 2500) {
            tryAutoStartRepeater();
        }

        syncPortalAuthWithApStations();

        // Repeater resilience: if upstream drops, retry connection and keep NAPT status honest.
        static uint32_t lastRepeaterReconnectAttempt = 0;
        if (repeater_active) {
            const bool staConnected = (WiFi.status() == WL_CONNECTED);

            // If STA is down, NAPT can't work: disable it so clients don't get "ghost" NAT.
            if (!staConnected && repeater_napt_enabled) {
                disableRepeaterNapt();
            }

            // If STA is back but NAPT is off, enable it again.
            if (staConnected && !repeater_napt_enabled) {
                enableRepeaterNapt();
            }

            // Periodically retry STA connection if disconnected (common after weak signal / AP reboot).
            if (!staConnected && (millis() - lastRepeaterReconnectAttempt > 8000)) {
                lastRepeaterReconnectAttempt = millis();

                repeater_saved_cfg_t cfg;
                if (loadRepeaterConfig(cfg) && cfg.upSsid.length() > 0) {
                    prntln("[Repeater] STA disconnected — retrying connect to «" + cfg.upSsid + "»…");
                    WiFi.persistent(false);
                    WiFi.mode(WIFI_AP_STA);
                    yield();
                    WiFi.begin(cfg.upSsid.c_str(), cfg.upPass.c_str());
                } else {
                    prntln("[Repeater] STA disconnected — no saved upstream config");
                }
            }
        }

        // Periodic repeater status logging
        static uint32_t lastRepeaterCheck = 0;
        if (repeater_active && (millis() - lastRepeaterCheck > 15000)) {
            lastRepeaterCheck = millis();
            prntln("[Repeater] Status: STA=(" + String(WiFi.SSID()) + " ch:" + String(WiFi.channel()) + 
                   " rssi:" + String(WiFi.RSSI()) + "dBm) | AP=" + String(WiFi.softAPSSID()) + 
                   " | NAPT=" + (repeater_napt_enabled ? "ON" : "OFF") + 
                   " | Clients=" + String(wifi_softap_get_station_num()));
        }

        // P0: never blocked by scan/sniff
        DIAG_PHASE(DIAG_P0, "WIFI", "http");
        server.handleClient();

        DIAG_PHASE(DIAG_P0, "WIFI", "dns");
        if (dns_captive_active) dns.processNextRequest();
        else if (repeater_dns_forward_active) processRepeaterDnsForwarder();

        // P1: drain promisc ring in main loop (bounded budget)
        DIAG_PHASE(DIAG_P1, "WIFI", "promisc");
        processPromiscQueue();
    }
}

extern "C" void ICACHE_RAM_ATTR promisc_rx_cb(uint8_t* buf, uint16_t len) {
    wifi::promiscRxFromIsr(buf, len);
}
