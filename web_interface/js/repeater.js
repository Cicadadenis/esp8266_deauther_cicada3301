var scanJson = { aps: [], stations: [] };
var repNetPickerEl = null;
var repWizardStep = 1;
var repSelectedNetIdx = -1;
var repLinkWasLost = false;
var repLinkWatchTimer = null;
var REP_WAIT_KEY = (typeof DEVICE_WAIT_KEY !== "undefined") ? DEVICE_WAIT_KEY : "repWaitReconnect";

function setStatus(t) {
    var el = getE("repStatus");
    if (el) el.textContent = t || "";
}

function formatStatusLine(s) {
    if (!s) return "";
    var t = "Upstream: " + (s.staConnected ? "подключено" : "нет");
    if (s.staSsid) t += " («" + s.staSsid + "»)";
    if (s.staIp && s.staIp !== "0.0.0.0") t += " · IP " + s.staIp;
    if (s.staChannel) t += " · ch " + s.staChannel;
    t += "  |  AP: " + (s.apActive ? "вкл" : "выкл");
    if (s.apSsid) t += " («" + s.apSsid + "»)";
    if (s.repeaterActive) t += "  |  мост активен";
    if (s.naptEnabled) t += "  |  NAT";
    return t;
}

function setWizardUi(step) {
    repWizardStep = step;
    var panels = [getE("repStep1"), getE("repStep2")];
    var badges = [getE("wizStep1"), getE("wizStep2")];
    var i;
    for (i = 0; i < panels.length; i++) {
        if (panels[i]) panels[i].classList.toggle("active", (i + 1) === step);
        if (badges[i]) {
            badges[i].classList.remove("active", "done");
            if (i + 1 === step) badges[i].classList.add("active");
            else if (i + 1 < step) badges[i].classList.add("done");
        }
    }
}

function goRepeaterStep(step, resetUpstream) {
    if (resetUpstream && step === 1) getFile("/repeater/cancel-upstream", function () { }, 6000);
    setWizardUi(step);
}

function ensureRepNetPicker() {
    if (repNetPickerEl) return repNetPickerEl;
    repNetPickerEl = document.createElement("select");
    repNetPickerEl.id = "repNetPicker";
    repNetPickerEl.setAttribute("data-cyber-skip", "1");
    repNetPickerEl.style.display = "none";
    document.body.appendChild(repNetPickerEl);
    return repNetPickerEl;
}

function openRepeaterNetworkPicker() {
    if (!scanJson.aps || scanJson.aps.length === 0) {
        showActionToast("Сети не найдены — обновите страницу", "warn");
        return;
    }

    var sel = ensureRepNetPicker();
    var html = "";
    var i;
    var ap;
    var ssid;
    var label;

    sel.innerHTML = "";
    for (i = 0; i < scanJson.aps.length; i++) {
        ap = scanJson.aps[i];
        ssid = (ap[0] || "").toString();
        label = (ssid || "(скрытая)") + " · ch " + ap[2] + " · " + ap[3] + " dBm · " + (ap[4] || "");
        html += "<option value=\"" + i + "\">" + esc(label) + "</option>";
    }
    sel.innerHTML = html;
    sel.setAttribute("data-cyber-title", "Выберите Wi‑Fi сеть");

    sel.onchange = function () {
        var idx = parseInt(this.value, 10);
        if (!isNaN(idx)) pickNet(idx);
    };

    if (typeof enhanceCyberSelect === "function") {
        sel.removeAttribute("data-cyber-done");
        enhanceCyberSelect(sel);
    }
    if (typeof openCyberPicker === "function") {
        openCyberPicker(sel);
    }
}

function drawNetworks() {
    var el = getE("netList");
    var hint = getE("netHint");
    if (!el) return;
    var html = "";
    if (!scanJson.aps || scanJson.aps.length === 0) {
        html = "<div class=hint style='padding:0.85rem;text-align:center'>Идёт автоскан Wi‑Fi… если список пуст — обновите страницу</div>";
        if (hint) hint.textContent = "";
        el.innerHTML = html;
        return;
    }
    if (hint) hint.textContent = "Найдено: " + scanJson.aps.length;

    for (var i = 0; i < scanJson.aps.length; i++) {
        var ap = scanJson.aps[i];
        var ssid = (ap[0] || "").toString();
        var ch = ap[2];
        var rssi = ap[3];
        var enc = ap[4];
        var selCls = (i === repSelectedNetIdx) ? " selected" : "";
        html += "<button type=button class='net" + selCls + "' onclick='pickNet(" + i + ")'>"
            + "<div class=net-main>"
            + "<div class=ssid title='" + esc(ssid) + "'>" + esc(ssid || "(скрытая)") + "</div>"
            + "<div class=meta>Канал " + esc(String(ch)) + " · " + esc(String(enc)) + " · " + esc(String(rssi)) + " dBm</div>"
            + "</div>"
            + "<span class=net-pick>Выбрать</span>"
            + "</button>";
    }
    el.innerHTML = html;
}

function pickNet(i) {
    if (!scanJson.aps || i < 0 || i >= scanJson.aps.length) return;
    var ap = scanJson.aps[i];
    var ssid = ap[0] || "";
    var enc = String(ap[4] || "");

    repSelectedNetIdx = i;

    getE("upSsid").value = ssid;
    drawNetworks();
    showActionToast("Сеть: " + (ssid || "(скрытая)"), "ok", 2000);

    if (enc.indexOf("OPEN") >= 0 || enc === "0" || enc === "") {
        getE("upPass").value = "";
    } else {
        getE("upPass").focus();
        showActionToast("Введите пароль от выбранной сети", "info", 2800);
    }
}

function reloadScanJson(done) {
    getFile("scan.json", function (res) {
        try {
            scanJson = JSON.parse(res);
            if (!scanJson.aps) scanJson.aps = [];
        } catch (e) {
            scanJson = { aps: [] };
        }
        drawNetworks();
        if (typeof done === "function") done();
    }, 12000);
}

function waitScanComplete(attempt, done) {
    if (attempt <= 0) {
        reloadScanJson(done);
        return;
    }
    getFile("scan.json", function (res) {
        try {
            var j = JSON.parse(res);
            if (j.aps && j.aps.length > 0) {
                scanJson = j;
                drawNetworks();
                if (typeof done === "function") done();
                return;
            }
        } catch (e) { /* retry */ }
        setTimeout(function () {
            waitScanComplete(attempt - 1, done);
        }, 1500);
    }, 8000);
}

function scanUpstream() {
    setStatus("Автоскан Wi‑Fi…");
    var btn = getE("btnApplyRepeater");
    if (btn) btn.disabled = true;
    getFile("/repeater/scan", function () {
        waitScanComplete(18, function () {
            setStatus("Выберите сеть и введите пароль");
            if (btn) btn.disabled = false;
            if (!scanJson.aps || scanJson.aps.length === 0) {
                showActionToast("Сети не найдены — обновите страницу", "warn", 3500);
            } else {
                showActionToast("Выберите сеть из списка", "info", 2500);
                openRepeaterNetworkPicker();
            }
        });
    }, 12000, "GET", function () {
        setStatus("");
        if (btn) btn.disabled = false;
        showActionToast("Не удалось запустить скан (устройство перезагрузилось?)", "err", 4500);
    });
}

function needsUpstreamPassword() {
    if (repSelectedNetIdx < 0 || !scanJson.aps || !scanJson.aps[repSelectedNetIdx]) return false;
    var enc = String(scanJson.aps[repSelectedNetIdx][4] || "");
    return !(enc.indexOf("OPEN") >= 0 || enc === "0" || enc === "");
}

function applyRepeater() {
    var upSsid = (getE("upSsid").value || "").trim();
    var upPass = (getE("upPass").value || "").trim();
    if (!upSsid) {
        showActionToast("Сначала выберите сеть", "err");
        goRepeaterStep(1, false);
        return;
    }
    if (needsUpstreamPassword() && !upPass) {
        showActionToast("Введите пароль Wi‑Fi", "err");
        getE("upPass").focus();
        goRepeaterStep(1, false);
        return;
    }

    var apSsid = (getE("apSsid").value || "").trim();
    var apPass = (getE("apPass").value || "").trim();

    if (!apSsid) {
        showActionToast("Введите имя точки доступа", "err");
        return;
    }
    if (apPass && apPass.length < 8) {
        showActionToast("Пароль AP: от 8 символов или оставьте пустым", "err");
        return;
    }

    var btn = getE("btnApplyRepeater");
    if (btn) btn.disabled = true;
    setStatus("Сохранение и запуск ретранслятора…");

    var qs = "/repeater/connect?upSsid=" + encodeURIComponent(upSsid)
        + "&upPass=" + encodeURIComponent(upPass)
        + "&apSsid=" + encodeURIComponent(apSsid)
        + "&apPass=" + encodeURIComponent(apPass);

    try { sessionStorage.setItem(REP_WAIT_KEY, "1"); } catch (e) { }
    markRepeaterReconnectWait();

    getFile(qs, function (res) {
        if (btn) btn.disabled = false;
        // /repeater/connect now ACKs immediately; Wi‑Fi will reconfigure and the browser may disconnect.
        showActionToast("Принято. Переподключитесь к Wi‑Fi — страница обновится сама", "ok", 6000);
        setStatus("Настройка моста… Подключитесь к точке доступа снова — страница обновится автоматически");
        beginRepeaterLinkWatch(true);
    }, 25000, "GET", function () {
        if (btn) btn.disabled = false;
        setStatus("");
        showActionToast("Таймаут применения", "err");
    });
}

function disconnectRepeater() {
    uiConfirm({
        title: "Отключить ретранслятор",
        message: "Остановить ретранслятор и вернуть обычную точку доступа?",
        confirmText: "Отключить",
        cancelText: "Отмена",
        danger: true,
        icon: "⏻"
    }).then(function (ok) {
        if (!ok) return;
        setStatus("Отключение…");
        getFile("/repeater/disconnect", function () {
            repUpstreamVerified = false;
            repVerifiedUpSsid = "";
            repVerifiedUpPass = "";
            goRepeaterStep(1, true);
            showActionToast("Ретранслятор остановлен", "ok", 2000);
            setTimeout(fetchStatus, 800);
        });
    });
}

function applyStatusFromJson(res) {
    try {
        var s = JSON.parse(res);
        setStatus(formatStatusLine(s));
        if (s.repeaterActive && s.staConnected) {
            goRepeaterStep(2, false);
            var msgEl = getE("repDoneMsg");
            if (msgEl && s.apSsid) {
                msgEl.innerHTML = "Активно: AP <strong>" + esc(s.apSsid) + "</strong>"
                    + " · upstream <strong>" + esc(s.staSsid || "") + "</strong>";
            }
        }
        return true;
    } catch (e) {
        return false;
    }
}

function fetchStatus() {
    getFile("/repeater/status.json", function (res) {
        if (!applyStatusFromJson(res)) setStatus("");
    }, 6000);
}

function markRepeaterReconnectWait() {
    repLinkWasLost = true;
    setStatus("Подключитесь к Wi‑Fi устройства снова — страница обновится автоматически");
}

function pingRepeater(ok, fail) {
    var xhr = new XMLHttpRequest();
    xhr.open("GET", "/repeater/status.json?_=" + Date.now(), true);
    xhr.timeout = 4000;
    xhr.onreadystatechange = function () {
        if (xhr.readyState !== 4) return;
        if (xhr.status === 200) {
            if (typeof ok === "function") ok(xhr.responseText);
        } else if (typeof fail === "function") {
            fail();
        }
    };
    xhr.onerror = function () {
        if (typeof fail === "function") fail();
    };
    xhr.ontimeout = function () {
        if (typeof fail === "function") fail();
    };
    xhr.send();
}

function pollRepeaterLink() {
    pingRepeater(function (res) {
        var waiting = false;
        try { waiting = sessionStorage.getItem(REP_WAIT_KEY) === "1"; } catch (e) { }

        if (repLinkWasLost || waiting) {
            try { sessionStorage.removeItem(REP_WAIT_KEY); } catch (e) { }
            location.reload();
            return;
        }

        applyStatusFromJson(res);
    }, function () {
        repLinkWasLost = true;
        setStatus("Нет связи с устройством — подключитесь к его Wi‑Fi снова");
    });
}

function beginRepeaterLinkWatch(forceLost) {
    if (forceLost) repLinkWasLost = true;
    if (repLinkWatchTimer) return;
    repLinkWatchTimer = setInterval(pollRepeaterLink, 2500);
    if (!window.__repOnlineHook) {
        window.__repOnlineHook = true;
        window.addEventListener("online", pollRepeaterLink);
    }
}

function load() {
    var apEl = getE("apSsid");
    if (apEl && !apEl.value) apEl.value = "cicada3301_relay";
    goRepeaterStep(1, false);

    var waitReconnect = false;
    try { waitReconnect = sessionStorage.getItem(REP_WAIT_KEY) === "1"; } catch (e) { }
    if (/[?&]wait=1(?:&|$)/.test(location.search || "")) waitReconnect = true;

    if (waitReconnect) markRepeaterReconnectWait();
    beginRepeaterLinkWatch(waitReconnect);

    reloadScanJson();
    pollRepeaterLink();
    setTimeout(scanUpstream, 300);
}
