/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

#include "SSIDs.h"

#include <LittleFS.h>
#include "settings.h"

uint16_t ssidListSize = SSID_LIST_SIZE_DEFAULT;

SSIDs::SSIDs() {
    list = new SimpleList<SSID>;
}

void SSIDs::load() {
    internal_removeAll();
    DynamicJsonBuffer jsonBuffer(4000);

    checkFile(FILE_PATH, str(SS_JSON_DEFAULT));
    JsonObject& obj = parseJSONFile(FILE_PATH, jsonBuffer);

    uint16_t size = SSID_LIST_SIZE_DEFAULT;

    if (obj.containsKey(str(SS_JSON_LISTSIZE))) size = obj[str(SS_JSON_LISTSIZE)];
    if (size < 1) size = 1;
    if (size > 255) size = 255;
    ssidListSize = size;

    JsonArray & arr = obj.get<JsonArray>(str(SS_JSON_SSIDS));

    for (uint32_t i = 0; i < arr.size() && i < SSID_LIST_SIZE; i++) {
        JsonArray& tmpArray = arr.get<JsonVariant>(i);
        internal_add(tmpArray.get<String>(0), tmpArray.get<bool>(1), tmpArray.get<int>(2));
    }

    prnt(SS_LOADED);
    prntln(FILE_PATH);
}

void SSIDs::load(String filepath) {
    String tmp = FILE_PATH;

    FILE_PATH = filepath;
    load();
    FILE_PATH = tmp;
}

void SSIDs::removeAll() {
    internal_removeAll();
    prntln(SS_CLEARED);
    changed = true;
}

void SSIDs::save(bool force) {
    if (!force && !changed) return;

    String buf = String();                              // create buffer

    buf += String(OPEN_CURLY_BRACKET) + String(DOUBLEQUOTES) + str(SS_JSON_RANDOM) + String(DOUBLEQUOTES) + String(
        DOUBLEPOINT) + b2s(randomMode) + String(COMMA); // {"random":false,
    buf += String(DOUBLEQUOTES) + str(SS_JSON_LISTSIZE) + String(DOUBLEQUOTES) + String(DOUBLEPOINT) +
           String(ssidListSize) + String(COMMA);        // "listSize":100,
    buf += String(DOUBLEQUOTES) + str(SS_JSON_SSIDS) + String(DOUBLEQUOTES) + String(DOUBLEPOINT) +
           String(OPEN_BRACKET);                        // "ssids":[

    if (!writeFile(FILE_PATH, buf)) {
        prnt(F_ERROR_SAVING);
        prntln(FILE_PATH);
        return;
    }
    buf = String(); // clear buffer

    String name;
    int    c = count();

    for (int i = 0; i < c; i++) {
        name = escape(getName(i));

        buf += String(OPEN_BRACKET) + String(DOUBLEQUOTES) + name + String(DOUBLEQUOTES) + String(COMMA); // ["name",
        buf += b2s(getWPA2(i)) + String(COMMA);                                                           // false,
        buf += String(getLen(i)) + String(CLOSE_BRACKET);                                                 // 12]

        if (i < c - 1) buf += COMMA;                                                                      // ,

        if (buf.length() >= 1024) {
            if (!appendFile(FILE_PATH, buf)) {
                prnt(F_ERROR_SAVING);
                prntln(FILE_PATH);
                return;
            }

            buf = String(); // clear buffer
        }
    }

    buf += String(CLOSE_BRACKET) + String(CLOSE_CURLY_BRACKET); // ]}

    if (!appendFile(FILE_PATH, buf)) {
        prnt(F_ERROR_SAVING);
        prntln(FILE_PATH);
        return;
    }

    prnt(SS_SAVED_IN);
    prntln(FILE_PATH);
    changed = false;
}

void SSIDs::save(bool force, String filepath) {
    String tmp = FILE_PATH;

    FILE_PATH = filepath;
    save(force);
    FILE_PATH = tmp;
}

void SSIDs::update() {
    if (randomMode) {
        if (currentTime - randomTime > randomInterval * 1000) {
            prntln(SS_RANDOM_INFO);

            for (int i = 0; i < SSID_LIST_SIZE; i++) {
                SSID newSSID;

                if (check(i)) newSSID = list->get(i);

                newSSID.name = String();
                newSSID.len  = 32;

                for (int i = 0; i < 32; i++) newSSID.name += char(random(32, 127));

                newSSID.wpa2 = random(0, 2);

                if (check(i)) list->replace(i, newSSID);
                else list->add(newSSID);
            }

            randomTime = currentTime;
            changed    = true;
        }
    }
}

String SSIDs::getName(int num) {
    return check(num) ? list->get(num).name : String();
}

bool SSIDs::getWPA2(int num) {
    return check(num) ? list->get(num).wpa2 : false;
}

int SSIDs::getLen(int num) {
    return check(num) ? list->get(num).len : 0;
}

void SSIDs::setWPA2(int num, bool wpa2) {
    SSID newSSID = list->get(num);

    newSSID.wpa2 = wpa2;
    list->replace(num, newSSID);
}

String SSIDs::getEncStr(int num) {
    if (getWPA2(num)) return "WPA2";
    else return "-";
}

void SSIDs::remove(int num) {
    if (!check(num)) return;

    internal_remove(num);
    prnt(SS_REMOVED);
    prntln(getName(num));
    changed = true;
}

String SSIDs::randomize(String name) {
    int ssidlen = name.length();

    if (ssidlen > 32) name = name.substring(0, 32);

    if (ssidlen < 32) {
        for (int i = ssidlen; i < 32; i++) {
            int rnd = random(3);

            if ((i < 29) && (rnd == 0)) { // ZERO WIDTH SPACE
                name += char(0xE2);
                name += char(0x80);
                name += char(0x8B);
                i    += 2;
            } else if ((i < 30) && (rnd == 1)) { // NO-BREAK SPACE
                name += char(0xC2);
                name += char(0xA0);
                i    += 1;
            } else {
                name += char(0x20); // SPACE
            }
        }
    }
    return name;
}

void SSIDs::add(String name, bool wpa2, int clones, bool force) {
    if (clones < 1) clones = 1;

    if (list->size() >= SSID_LIST_SIZE) {
        if (force) {
            internal_remove(0);
        } else {
            prntln(SS_ERROR_FULL);
            return;
        }
    }

    if (clones > SSID_LIST_SIZE) clones = SSID_LIST_SIZE;

    for (int i = 0; i < clones; i++) {
        if (list->size() >= SSID_LIST_SIZE) {
            if (!force) break;
            internal_remove(0);
        }

        if (clones > 1) internal_add(randomize(name), wpa2, 32);
        else internal_add(name, wpa2, name.length() > 32 ? 32 : name.length());

        if ((i & 1) == 1) yield();
    }

    prnt(SS_ADDED);
    prntln(name);
    changed = true;
}

void SSIDs::cloneSelected(bool force) {
    int sel = accesspoints.selected();

    if (sel <= 0) {
        prntln(F("Clone: select AP(s) on Scan tab first"));
        return;
    }

    int slots = (int)ssidListSize - (int)list->size();

    if (!force && slots <= 0) {
        prntln(SS_ERROR_FULL);
        return;
    }

    int clonesPerAp = 1;
    int budget    = force ? (int)ssidListSize : slots;

    if (budget < 1) budget = 1;

    clonesPerAp = budget / sel;

    if (clonesPerAp < 1) clonesPerAp = 1;

    // Was: clonesPerAp could be 100+ on empty list → hundreds of randomize() → reboot
    if (clonesPerAp > 250) clonesPerAp = 250;

    int added   = 0;
    int apCount = accesspoints.count();

    for (int i = 0; i < apCount; i++) {
        if (!accesspoints.getSelected(i)) continue;

        String ssid = accesspoints.getSSID(i);

        ssid.trim();

        if (ssid.length() == 0) continue;

        int before = list->size();

        add(ssid, accesspoints.getEnc(i) != ENC_TYPE_NONE, clonesPerAp, force);
        added += list->size() - before;
        yield();
    }

    prnt(F("Clone: added "));
    prntln(String(added));
}

bool SSIDs::getRandom() {
    return randomMode;
}

void SSIDs::replace(int num, String name, bool wpa2) {
    if (!check(num)) return;

    int len = name.length();

    if (len > 32) len = 32;
    SSID newSSID;

    newSSID.name = randomize(name);
    newSSID.wpa2 = wpa2;
    newSSID.len  = (uint8_t)len;
    list->replace(num, newSSID);

    prnt(SS_REPLACED);
    prntln(name);
    changed = true;
}

void SSIDs::print(int num) {
    print(num, true, false);
}

void SSIDs::print(int num, bool header, bool footer) {
    if (!check(num)) return;

    if (header) {
        prntln(SS_TABLE_HEADER);
        prntln(SS_TABLE_DIVIDER);
    }

    prnt(leftRight(String(), (String)num, 2));
    prnt(leftRight(String(SPACE), getEncStr(num), 5));
    prntln(leftRight(String(SPACE) + getName(num), String(), 33));

    if (footer) prntln(SS_TABLE_DIVIDER);
}

void SSIDs::printAll() {
    prntln(SS_HEADER);
    int c = count();

    if (c == 0) prntln(SS_ERROR_EMPTY);
    else
        for (int i = 0; i < c; i++) print(i, i == 0, i == c - 1);
}

int SSIDs::count() {
    return list->size();
}

bool SSIDs::check(int num) {
    return num >= 0 && num < count();
}

void SSIDs::enableRandom(uint32_t randomInterval) {
    randomMode            = true;
    SSIDs::randomInterval = randomInterval;
    prntln(SS_RANDOM_ENABLED);
    update();
}

void SSIDs::disableRandom() {
    randomMode = false;
    internal_removeAll();
    prntln(SS_RANDOM_DISABLED);
}

void SSIDs::internal_add(String name, bool wpa2, int len) {
    if (len > 32) {
        name = name.substring(0, 32);
        len  = 32;
    }

    name = fixUtf8(name);

    SSID newSSID;

    newSSID.name = name;
    newSSID.wpa2 = wpa2;
    newSSID.len  = (uint8_t)len;

    list->add(newSSID);
}

void SSIDs::internal_remove(int num) {
    list->remove(num);
}

void SSIDs::internal_removeAll() {
    list->clear();
}

void SSIDs::setListSize(uint16_t size) {
    if (size < 1) size = 1;
    if (size > 255) size = 255;

    ssidListSize = size;

    while (list->size() > ssidListSize) internal_remove(0);

    changed = true;
}

uint16_t SSIDs::getListSize() {
    return ssidListSize;
}