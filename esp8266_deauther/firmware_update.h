/* OTA firmware upload via web UI */
#pragma once

#include <ESP8266WebServer.h>

typedef bool (*FirmwareAuthFn)(void);

void firmwareUpdateRegister(ESP8266WebServer& server, FirmwareAuthFn authCheck);
