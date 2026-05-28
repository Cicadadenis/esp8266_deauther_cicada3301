/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

var nameJson = [];
var scanJson = { aps: [], stations: [] };
var filterApIndex = -1;
var scheduledScanTimer = null;

function renderSignal(rssi) {
	var width = parseInt(rssi, 10) + 130;
	if (isNaN(width)) width = 0;
	if (width < 0) width = 0;
	if (width > 100) width = 100;

	var color;
	if (width < 50) color = "meter_red";
	else if (width < 70) color = "meter_orange";
	else color = "meter_green";

	return "<div class='meter_background'><div class='meter_forground " + color + "' style='width: " + width + "%;'><div class='meter_value'>" + esc(rssi) + "</div></div></div>";
}

function rssiGaugeSvg(rssi) {
	var r = parseInt(rssi, 10);
	if (isNaN(r)) r = -80;
	var min = -95;
	var max = -35;
	var t = (r - min) / (max - min);
	if (t < 0) t = 0;
	if (t > 1) t = 1;
	var deg = 225 - t * 270;
	var rad = deg * Math.PI / 180;
	var cx = 34;
	var cy = 34;
	var nx = (cx + Math.cos(rad) * 22).toFixed(1);
	var ny = (cy + Math.sin(rad) * 22).toFixed(1);
	var ticks = "";
	var i;
	var a;
	var x1;
	var y1;
	var x2;
	var y2;
	for (i = 0; i <= 8; i++) {
		a = (225 - (i / 8) * 270) * Math.PI / 180;
		x1 = cx + Math.cos(a) * 26;
		y1 = cy + Math.sin(a) * 26;
		x2 = cx + Math.cos(a) * 30;
		y2 = cy + Math.sin(a) * 30;
		ticks += '<line x1="' + x1.toFixed(1) + '" y1="' + y1.toFixed(1) + '" x2="' + x2.toFixed(1)
			+ '" y2="' + y2.toFixed(1) + '" stroke="rgba(255,255,255,0.35)" stroke-width="1"/>';
	}
	return '<svg viewBox="0 0 68 68" aria-hidden="true">'
		+ '<circle cx="' + cx + '" cy="' + cy + '" r="30" fill="#0a1020" stroke="rgba(80,120,180,0.5)" stroke-width="1.5"/>'
		+ ticks
		+ '<circle cx="' + cx + '" cy="' + cy + '" r="3" fill="#fff"/>'
		+ '<line x1="' + cx + '" y1="' + cy + '" x2="' + nx + '" y2="' + ny
		+ '" stroke="#ff8c2a" stroke-width="2.5" stroke-linecap="round"/>'
		+ '<text x="' + cx + '" y="48" text-anchor="middle" fill="#fff" font-size="9" font-family="JetBrains Mono,monospace">'
		+ esc(String(r)) + ' dBm</text></svg>';
}

function rssiToBarGradient(rssi) {
	var r = parseInt(rssi, 10);
	if (isNaN(r)) r = -90;
	if (r <= -78) return "linear-gradient(90deg,#ff3366 0%,#ff6b8a 100%)";
	if (r <= -70) return "linear-gradient(90deg,#ff8800 0%,#ffcc44 100%)";
	if (r <= -62) return "linear-gradient(90deg,#00c8ff 0%,#00f0ff 100%)";
	return "linear-gradient(90deg,#6d5cff 0%,#00f0ff 100%)";
}

function rssiGlowColor(rssi) {
	var r = parseInt(rssi, 10);
	if (isNaN(r)) r = -90;
	if (r <= -78) return "rgba(255,51,102,0.55)";
	if (r <= -70) return "rgba(255,170,0,0.50)";
	if (r <= -62) return "rgba(0,240,255,0.55)";
	return "rgba(122,92,255,0.55)";
}

var scanConeColors = ["#c026d3", "#f97316", "#2563eb", "#06b6d4", "#7c3aed", "#22d3ee", "#e879f9", "#38bdf8"];

/* Горизонтальное «растягивание» платформы радара (rx > ry) */
var SCAN_PLATFORM_RX = 1.42;
var SCAN_PLATFORM_RY = 0.30;

function scanConeChannelAngle(ch) {
	var c = parseInt(ch, 10);
	if (isNaN(c) || c < 1) c = 1;
	if (c > 14) c = 14;
	return ((c - 1) / 13) * Math.PI * 2 - Math.PI / 2;
}

function scanApLabel(ap) {
	var ssid = (ap[0] || "").trim();
	if (!ssid || ssid === "*HIDDEN*" || ssid.indexOf("HIDDEN") >= 0) {
		ssid = (ap[1] || "").trim() || "(скрытый)";
	}
	if (ssid.length > 20) ssid = ssid.substring(0, 20) + "\u2026";
	return ssid;
}

function drawConeSsidLabel(ctx, x, y, text) {
	if (!text) return;
	ctx.save();
	ctx.font = "600 11px JetBrains Mono, monospace";
	ctx.textAlign = "center";
	ctx.textBaseline = "bottom";
	ctx.lineWidth = 3.5;
	ctx.strokeStyle = "rgba(0,0,0,0.9)";
	ctx.strokeText(text, x, y);
	ctx.fillStyle = "rgba(255, 215, 120, 0.98)";
	ctx.fillText(text, x, y);
	ctx.restore();
}

function drawScanPlatform(ctx, cx, cy, R) {
	var rx = R * SCAN_PLATFORM_RX;
	var ry = R * SCAN_PLATFORM_RY;
	var plat = ctx.createRadialGradient(cx, cy - ry * 0.2, 0, cx, cy, Math.max(rx, ry) * 1.12);
	plat.addColorStop(0, "rgba(0,240,255,0.22)");
	plat.addColorStop(0.55, "rgba(34,85,255,0.14)");
	plat.addColorStop(1, "rgba(0,0,0,0)");
	ctx.fillStyle = plat;
	ctx.beginPath();
	ctx.ellipse(cx, cy, rx, ry, 0, 0, Math.PI * 2);
	ctx.fill();

	ctx.strokeStyle = "rgba(0,240,255,0.20)";
	ctx.lineWidth = 1;
	var ring;
	for (ring = 1; ring <= 5; ring++) {
		ctx.beginPath();
		ctx.ellipse(cx, cy, rx * (ring / 5), ry * (ring / 5), 0, 0, Math.PI * 2);
		ctx.stroke();
	}

	var rays = 10;
	var i;
	for (i = 0; i < rays; i++) {
		var a = (i / rays) * Math.PI * 2 - Math.PI / 2;
		ctx.beginPath();
		ctx.moveTo(cx, cy);
		ctx.lineTo(cx + Math.cos(a) * rx, cy + Math.sin(a) * ry);
		ctx.stroke();
	}

	ctx.fillStyle = "rgba(0,240,255,0.85)";
	ctx.shadowColor = "#00f0ff";
	ctx.shadowBlur = 14;
	ctx.beginPath();
	ctx.ellipse(cx, cy, 5, 2.2, 0, 0, Math.PI * 2);
	ctx.fill();
	ctx.shadowBlur = 0;
}

function drawWavyRing(ctx, bx, by, rx, ry, alpha, phase) {
	var segs = 36;
	var s;
	ctx.beginPath();
	for (s = 0; s <= segs; s++) {
		var a = (s / segs) * Math.PI * 2;
		var wob = Math.sin(a * 5 + phase) * (rx * 0.06) + Math.cos(a * 3 - phase * 0.7) * (rx * 0.04);
		var px = bx + Math.cos(a) * (rx + wob);
		var py = by + Math.sin(a) * (ry + wob * 0.35);
		if (s === 0) ctx.moveTo(px, py);
		else ctx.lineTo(px, py);
	}
	ctx.closePath();
	ctx.strokeStyle = "rgba(255,255,255," + alpha + ")";
	ctx.lineWidth = 0.75;
	ctx.stroke();
}

function drawVolumeCone(ctx, bx, by, coneH, coneW, col, phase) {
	var steps = 28;
	var i;
	var topY = by - coneH;

	ctx.save();
	ctx.shadowColor = col;
	ctx.shadowBlur = 22;

	ctx.beginPath();
	for (i = 0; i <= steps; i++) {
		var t = i / steps;
		var y = by - t * coneH;
		var rx = coneW * Math.pow(1 - t, 0.72);
		var x = bx - rx;
		if (i === 0) ctx.moveTo(x, y);
		else ctx.lineTo(x, y);
	}
	ctx.lineTo(bx, topY - 3);
	for (i = steps; i >= 0; i--) {
		var t = i / steps;
		var y = by - t * coneH;
		var rx = coneW * Math.pow(1 - t, 0.72);
		ctx.lineTo(bx + rx, y);
	}
	ctx.closePath();

	var body = ctx.createLinearGradient(bx, by, bx, topY);
	body.addColorStop(0, col + "55");
	body.addColorStop(0.35, col + "99");
	body.addColorStop(0.72, col + "cc");
	body.addColorStop(1, col);
	ctx.fillStyle = body;
	ctx.fill();
	ctx.restore();

	var rings = 7;
	var r;
	for (r = 1; r <= rings; r++) {
		var rt = r / (rings + 1);
		var ry = by - rt * coneH;
		var rx = coneW * Math.pow(1 - rt, 0.72);
		drawWavyRing(ctx, bx, ry, rx, rx * 0.36, 0.32 - rt * 0.12, phase + r * 0.9);
	}

	ctx.save();
	ctx.globalCompositeOperation = "lighter";
	ctx.fillStyle = col + "44";
	ctx.shadowColor = col;
	ctx.shadowBlur = 16;
	ctx.beginPath();
	ctx.ellipse(bx, by + 1, coneW * 0.62, coneW * 0.22, 0, 0, Math.PI * 2);
	ctx.fill();
	ctx.restore();
	ctx.shadowBlur = 0;
}

function drawScanCones() {
	var c = getE("scanCones");
	if (!c) return;
	var ctx = c.getContext("2d");
	var w = c.clientWidth || 320;
	var h = c.clientHeight || 220;
	var dpr = Math.min(2, window.devicePixelRatio || 1);
	c.width = Math.floor(w * dpr);
	c.height = Math.floor(h * dpr);
	ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

	ctx.clearRect(0, 0, w, h);
	var cx = w * 0.5;
	var cy = h * 0.62;
	var R = Math.min(w * 0.50, h * 0.58);
	var phase = Date.now() * 0.0018;
	var platRx = R * SCAN_PLATFORM_RX;
	var platRy = R * SCAN_PLATFORM_RY;

	drawScanPlatform(ctx, cx, cy, R);

	if (!scanJson.aps || scanJson.aps.length === 0) {
		ctx.fillStyle = "rgba(180,190,230,0.6)";
		ctx.font = "12px JetBrains Mono, monospace";
		ctx.textAlign = "center";
		ctx.fillText("Скан Wi‑Fi → появятся пики сигнала", cx, cy - R * 0.35);
		return;
	}

	var items = [];
	var i;
	for (i = 0; i < scanJson.aps.length; i++) items.push({ idx: i, ap: scanJson.aps[i] });

	var cones = [];
	for (i = 0; i < items.length; i++) {
		var ap = items[i].ap;
		var rssi = parseInt(ap[3], 10);
		if (isNaN(rssi)) rssi = -90;
		var strength = Math.min(1, Math.max(0.12, (rssi + 95) / 65));
		var ang = scanConeChannelAngle(ap[2]) + ((i % 3) - 1) * 0.08;
		var dist = R * (0.22 + (1 - strength) * 0.48);
		var bx = cx + Math.cos(ang) * dist * (platRx / R);
		var by = cy + Math.sin(ang) * dist * (platRy / R);
		cones.push({
			bx: bx,
			by: by,
			h: 28 + strength * 72,
			w: 14 + strength * 22,
			col: scanConeColors[i % scanConeColors.length],
			label: scanApLabel(ap),
			depth: by
		});
	}

	cones.sort(function (a, b) { return a.depth - b.depth; });

	var maxShow = Math.min(cones.length, 8);
	for (i = 0; i < maxShow; i++) {
		var c0 = cones[i];
		drawVolumeCone(ctx, c0.bx, c0.by, c0.h, c0.w, c0.col, phase + i * 1.1);
	}

	var sweep = (Date.now() % 5000) / 5000;
	var sa = sweep * Math.PI * 2 - Math.PI / 2;
	ctx.fillStyle = "rgba(0,240,255,0.07)";
	ctx.beginPath();
	ctx.moveTo(cx, cy);
	ctx.ellipse(cx, cy, platRx, platRy, 0, sa - 0.42, sa + 0.06);
	ctx.closePath();
	ctx.fill();

	for (i = 0; i < maxShow; i++) {
		var c1 = cones[i];
		drawConeSsidLabel(ctx, c1.bx, c1.by - c1.h - 8, c1.label);
	}
}

function sanitizeScanJson() {
	if (!scanJson.aps) scanJson.aps = [];
	if (!scanJson.stations) scanJson.stations = [];
	var max = scanJson.aps.length;
	var clean = [];
	var i;
	for (i = 0; i < scanJson.stations.length; i++) {
		if (getStationApIndex(scanJson.stations[i]) >= 0) clean.push(scanJson.stations[i]);
	}
	scanJson.stations = clean;
	normalizeFilterApIndex();
}

function getStationApIndex(st) {
	var ap = parseInt(st[5], 10);
	if (isNaN(ap) || ap < 0 || ap >= scanJson.aps.length) return -1;
	return ap;
}

function normalizeFilterApIndex() {
	if (filterApIndex >= scanJson.aps.length) filterApIndex = -1;
	var sel = getSelectedApIndices();
	if (sel.length === 1) filterApIndex = sel[0];
	else if (filterApIndex >= 0 && sel.indexOf(filterApIndex) < 0) filterApIndex = -1;
}

function countClientsForAp(apIdx) {
	var n = 0;
	var i;
	for (i = 0; i < scanJson.stations.length; i++) {
		if (getStationApIndex(scanJson.stations[i]) === apIdx) n++;
	}
	return n;
}

function getSelectedApIndices() {
	var sel = [];
	for (var i = 0; i < scanJson.aps.length; i++) {
		if (scanJson.aps[i][scanJson.aps[i].length - 1]) sel.push(i);
	}
	return sel;
}

function getFilterApIndex() {
	if (getE("filterByAp") && !getE("filterByAp").checked) return -1;
	if (filterApIndex >= 0 && filterApIndex < scanJson.aps.length) return filterApIndex;
	var selected = getSelectedApIndices();
	if (selected.length === 1) return selected[0];
	return -1;
}

function clearStationFilter() {
	filterApIndex = -1;
	if (getE("filterByAp")) getE("filterByAp").checked = false;
	drawScan();
}

function quoteCmd(s) {
	return (s || "").replace(/\\/g, "\\\\").replace(/"/g, '\\"');
}

function selectOnlyAp(apIdx, done) {
	getFile("run?cmd=deselect ap", function () {
		getFile("run?cmd=select ap " + apIdx, function () {
			var j;
			for (j = 0; j < scanJson.aps.length; j++) {
				scanJson.aps[j][scanJson.aps[j].length - 1] = (j === apIdx);
			}
			filterApIndex = apIdx;
			if (getE("filterByAp")) getE("filterByAp").checked = true;
			if (done) done();
		});
	});
}

var HS_CAPTURE_SEC = 40;

function scanClientsForAp(apIdx) {
	if (scanJson.aps.length < 1) {
		showMessage("Сначала: Скан Wi-Fi Сети");
		return;
	}
	if (apIdx < 0 || apIdx >= scanJson.aps.length) return;

	selectOnlyAp(apIdx, function () {
		drawScan();
		filterApIndex = apIdx;
		if (getE("filterByAp")) getE("filterByAp").checked = true;
		var sec = getStationScanSeconds();
		var ap = scanJson.aps[apIdx];
		showReconnectModal({
			title: "Скан клиентов (AP)",
			sec: sec,
			apName: ap[0],
			channel: ap[2],
			keepApConnected: true,
			onConfirm: runStationScan
		});
	});
}

function findNameByMac(mac) {
	for (var i = 0; i < nameJson.length; i++) {
		if (nameJson[i][0] === mac) return i;
	}
	return -1;
}

function isPrivateMac(mac) {
	if (!mac || mac.length < 2) return false;
	var b = parseInt(mac.substring(0, 2), 16);
	return !isNaN(b) && (b & 0x02) !== 0;
}

function savedDeviceName(mac) {
	var idx = findNameByMac(mac);
	if (idx < 0) return "";
	var n = nameJson[idx][2];
	return n && n.length > 0 ? n : "";
}

function deviceLabelFromMac(mac, vendor, customName) {
	if (customName && customName.length > 0) return customName;
	if (vendor && vendor.length > 0) return vendor;
	if (isPrivateMac(mac)) return "Приватный MAC";
	return "Неизвестно";
}

function stationDeviceLabel(st) {
	return deviceLabelFromMac(st[0], st[3], savedDeviceName(st[0]) || st[2]);
}

function apDeviceLabel(ap) {
	return deviceLabelFromMac(ap[5], ap[6], savedDeviceName(ap[5]) || ap[1]);
}

function saveApToEeprom(apIdx) {
	if (scanJson.aps.length < 1 || apIdx < 0 || apIdx >= scanJson.aps.length) return;
	var ap = scanJson.aps[apIdx];
	var ch = ap[2];
	var label = quoteCmd(ap[1] || ap[0]);
	var mac = ap[5];
	var alreadySaved = findNameByMac(mac) >= 0;

	setScanStatus("Сохранение AP #" + apIdx + "…");

	function persistNames() {
		getFile("run?cmd=save names", function () {
			getFile("run?cmd=save settings", function () {
				getFile("names.json", function (res) {
					nameJson = JSON.parse(res);
					scanJson.aps[apIdx][1] = ap[0];
					drawScan();
					drawNames();
					showMessage("AP #" + apIdx + " сохранена");
					setScanStatus("Сохранено: канал " + ch + ", AP в «Сохранённые устройства»" + (alreadySaved ? " (обновлено)" : ""));
				});
			});
		});
	}

	selectOnlyAp(apIdx, function () {
		getFile("run?cmd=set channel " + ch, function () {
			if (alreadySaved) {
				persistNames();
				return;
			}
			if (nameJson.length >= 25) {
				showMessage("Device Name List is full!");
				return;
			}
			getFile('run?cmd=add name "' + label + '" -ap ' + apIdx + " -s", function () {
				scanJson.aps[apIdx][1] = ap[0];
				persistNames();
			});
		});
	});
}

function hsCapLink(label) {
	return '<a href="handshake.cap" download>' + esc(label || ".cap") + "</a>";
}

function setScanStatus(msg) {
	var el = getE("scanStatus");
	if (el) el.textContent = msg;
}

function setScanStatusHtml(html) {
	var el = getE("scanStatus");
	if (el) el.innerHTML = html;
}

function getOnlineStatus(timeStr) {
	var t = (timeStr || "").toLowerCase();
	if (t.indexOf("sec") >= 0) return { cls: "online-yes", label: "онлайн" };
	if (t.indexOf("min") >= 0) return { cls: "online-maybe", label: "недавно" };
	return { cls: "online-no", label: "давно" };
}

function getSecurityHints(enc) {
	enc = (enc || "").toUpperCase();
	var wps = "н/д";
	var pmf = "н/д";
	if (enc === "-" || enc === "") {
		wps = "—";
		pmf = "—";
	} else if (enc.indexOf("WPA2") >= 0) {
		wps = "н/д";
		pmf = "? (не в scan ESP)";
	} else if (enc.indexOf("WPA") >= 0) {
		pmf = "?";
	}
	return { wps: wps, pmf: pmf };
}

function exportScan() {
	var blob = new Blob([JSON.stringify(scanJson, null, 2)], { type: "application/json" });
	var a = document.createElement("a");
	a.href = URL.createObjectURL(blob);
	a.download = "scan-" + new Date().toISOString().slice(0, 19).replace(/:/g, "-") + ".json";
	a.click();
	URL.revokeObjectURL(a.href);
	showMessage("scan.json экспортирован");
}

function drawChannelMap() {
	var el = getE("channelMap");
	if (!el) return;
	var counts = [];
	var max = 0;
	var ch;
	var i;
	for (ch = 1; ch <= 14; ch++) counts[ch] = 0;
	for (i = 0; i < scanJson.aps.length; i++) {
		ch = parseInt(scanJson.aps[i][2], 10);
		if (ch >= 1 && ch <= 14) {
			counts[ch]++;
			if (counts[ch] > max) max = counts[ch];
		}
	}
	var html = "<div class='channel-map'>";
	for (ch = 1; ch <= 14; ch++) {
		var n = counts[ch] || 0;
		var h = max > 0 ? Math.round((n / max) * 40) : 0;
		html += "<div class='ch-cell" + (n > 0 ? " busy" : "") + "' title='Канал " + ch + ": " + n + " AP'>"
			+ "<div class='ch-num'>" + ch + "</div>"
			+ (n > 0 ? "<div class='ch-bar' style='height:" + h + "px'></div><div style='font-size:0.62rem;margin-top:2px'>" + n + "</div>" : "")
			+ "</div>";
	}
	html += "</div>";
	el.innerHTML = html;
}

function drawRssiChart() {
	var el = getE("rssiChart");
	if (!el) return;
	if (scanJson.aps.length === 0) {
		el.innerHTML = "<p style='opacity:0.8'>Нет AP — сначала скан</p>";
		return;
	}
	var items = [];
	var html = "";
	var i;
	var name;
	var rssi;
	var w;
	var ap;
	for (i = 0; i < scanJson.aps.length; i++) items.push({ idx: i, ap: scanJson.aps[i] });
	items.sort(function (a, b) { return parseInt(b.ap[3], 10) - parseInt(a.ap[3], 10); });
	for (i = 0; i < items.length && i < 24; i++) {
		ap = items[i].ap;
		rssi = parseInt(ap[3], 10);
		if (isNaN(rssi)) rssi = -90;
		w = Math.min(100, Math.max(6, rssi + 130));
		name = ap[1] || ap[0];
		if (name.length > 14) name = name.substring(0, 14) + "…";
		var grad = rssiToBarGradient(rssi);
		var glow = rssiGlowColor(rssi);
		html += "<div class='rssi-row' title='" + esc(ap[0]) + " ch" + ap[2] + "'>"
			+ "<div class='rssi-top'><span class='rssi-ssid'>" + esc(name) + "</span><span class='rssi-db'>" + rssi + " dBm</span></div>"
			+ "<div class='rssi-bar-bg'><div class='rssi-bar-fg' style='width:" + w + "%; background:" + grad + "; box-shadow:0 0 12px " + glow + ";'></div></div>"
			+ "</div>";
	}
	el.innerHTML = html;
	drawScanCones();
}

function toggleScheduledScan() {
	if (scheduledScanTimer) {
		clearInterval(scheduledScanTimer);
		scheduledScanTimer = null;
		getE("schedScanBtn").innerHTML = "Вкл";
		setScanStatus("Автоскан выключен");
		return;
	}
	if (getSelectedApIndices().length !== 1) {
		showMessage("Выберите одну AP для автоскана");
		return;
	}
	var min = parseInt(getE("schedScanMin").value, 10) || 5;
	if (min < 1) min = 1;
	getE("schedScanBtn").innerHTML = "Выкл";
	setScanStatus("Автоскан каждые " + min + " мин");
	scheduledScanTimer = setInterval(function () {
		if (getSelectedApIndices().length === 1) scan(1);
	}, min * 60 * 1000);
}

function captureHandshakeForStation(stIdx) {
	var apIdx = getStationApIndex(scanJson.stations[stIdx]);
	if (apIdx < 0) {
		var sel = getSelectedApIndices();
		if (sel.length === 1) apIdx = sel[0];
		else {
			showMessage("Выберите AP или клиента с известной AP");
			return;
		}
	}
	captureHandshake(apIdx, stIdx);
}

function runHandshakeCapture(apIdx, stIdx) {
	var sec = HS_CAPTURE_SEC;
	var cmd = "capture handshake -ap " + apIdx + " -t " + sec + "s";
	if (stIdx >= 0) cmd += " -st " + stIdx;

	var hint = stIdx >= 0
		? "HS клиент #" + stIdx + " / AP #" + apIdx
		: "HS AP #" + apIdx;
	setScanStatus(hint + " ~" + sec + " с — WiFi ESP выключится, потом снова cicada3301");
	selectOnlyAp(apIdx, function () {
		getFile("run?cmd=stop attack", function () {
			getFile("run?cmd=stop scan", function () {
				getFile("run?cmd=save scan", function () {
					getFile("run?cmd=" + cmd, function () {
						setScanStatus("Захват HS ~" + sec + " с — WiFi ESP выключится, потом cicada3301");
						pollHandshakeStatus(0, sec);
					});
				});
			});
		});
	});
}

function captureHandshake(apIdx, stIdx) {
	if (scanJson.aps.length < 1) {
		showMessage("Сначала: Скан Wi-Fi Сети");
		return;
	}
	var ap = scanJson.aps[apIdx];
	var cfg = {
		title: "Захват handshake (HS)",
		sec: HS_CAPTURE_SEC,
		apName: ap ? ap[0] : "",
		channel: ap ? ap[2] : "",
		warning: "Только своя тестовая AP! PMF/WPA3 часто блокируют захват.",
		keepApConnected: false,
		apOffDuringCapture: true,
		onConfirm: function () { runHandshakeCapture(apIdx, stIdx); }
	};
	if (stIdx >= 0 && scanJson.stations[stIdx]) {
		cfg.clientMac = scanJson.stations[stIdx][0];
		cfg.clientLabel = "Клиент #" + stIdx;
	}
	showReconnectModal(cfg);
}

function formatHsStatus(hs) {
	return "HS: M1=" + (hs.m1 ? "✓" : "·") + " M2=" + (hs.m2 ? "✓" : "·") + " M3=" + (hs.m3 ? "✓" : "·") + " M4=" + (hs.m4 ? "✓" : "·");
}

function pollHandshakeStatus(attempt, maxSec) {
	var maxAttempts = Math.ceil(maxSec / 2) + 30;
	if (attempt > maxAttempts) {
		setScanStatus("Проверка HS — подключитесь к cicada3301 и нажмите «Проверить HS»");
		checkHandshakeStatus();
		return;
	}
	setTimeout(function () {
		getFile("handshake.json", function (res) {
			var hs;
			try {
				hs = JSON.parse(res);
			} catch (e) {
				setScanStatus("Захват HS… (~" + maxSec + " с, попытка " + (attempt + 1) + ")");
				pollHandshakeStatus(attempt + 1, maxSec);
				return;
			}
			if (hs.active) {
				var extra = hs.frames ? " · кадров " + hs.frames : "";
				setScanStatus(formatHsStatus(hs) + " — захват…" + extra);
				pollHandshakeStatus(attempt + 1, maxSec);
				return;
			}
			if (hs.complete || hs.result === "ok") {
				setScanStatusHtml("✓ Handshake пойман — " + hsCapLink("скачать handshake.cap"));
				showMessage("Handshake OK — файл handshake.cap готов");
				return;
			}
			if (hs.m2 && !hs.m1) {
				setScanStatusHtml(
					"△ Только M2 (mask=2) — " + hsCapLink(".cap") + " · сразу повторите HS на том же клиенте"
				);
				showMessage("Пойман M2 — нужен M1. Запустите HS ещё раз");
				return;
			}
			if (hs.result === "partial" || hs.m2 || (hs.mask && hs.mask > 0)) {
				setScanStatusHtml("△ Handshake частичный (mask " + hs.mask + ") — " + hsCapLink(".cap"));
				showMessage("Handshake частичный — mask " + hs.mask);
				return;
			}
			setScanStatus("✗ Handshake не пойман (mask 0). Нажмите HS у клиента в списке · PMF/WPA3 мешают");
			showMessage("Handshake не пойман — выберите клиента в таблице и HS");
		}, 8000, "GET", function () {
			setScanStatus("Захват HS… WiFi ESP выкл (~" + maxSec + " с). Потом подключитесь к cicada3301");
			pollHandshakeStatus(attempt + 1, maxSec);
		});
	}, 2000);
}

function checkHandshakeStatus() {
	getFile("handshake.json", function (res) {
		try {
			var hs = JSON.parse(res);
			if (hs.active) {
				setScanStatus(formatHsStatus(hs) + " — захват ещё идёт");
			} else if (hs.complete || hs.result === "ok") {
				setScanStatusHtml(
					"✓ Последний HS OK — " + hsCapLink("handshake.cap") + " · " + esc(formatHsStatus(hs))
				);
				showMessage("Handshake OK");
			} else if (hs.m2 && !hs.m1) {
				setScanStatusHtml(
					"△ Последний HS: только M2 — повторите HS на клиенте · " + hsCapLink(".cap")
				);
			} else if (hs.result === "partial" || (hs.mask && hs.mask > 0)) {
				setScanStatusHtml("△ Последний HS частичный (mask " + hs.mask + ") — " + hsCapLink(".cap"));
			} else {
				setScanStatus("✗ Последний HS не пойман — HS на строке клиента (не только AP)");
			}
		} catch (e) {
			setScanStatus("Нет handshake.json — сначала нажмите HS");
		}
	}, 5000, "GET", function () {
		setScanStatus("Нет связи с ESP — подключитесь к cicada3301");
	});
}

function deauthStation(stId) {
	uiConfirm({
		title: "Deauth клиента",
		message: "Deauth станции #" + stId + " ~30 с.\nТолько своя лабораторная AP!",
		confirmText: "Подтвердить",
		cancelText: "Отмена",
		danger: true,
		icon: "⚡"
	}).then(function (ok) {
		if (!ok) return;
		getFile("run?cmd=stop attack", function () {
			getFile("run?cmd=deselect stations", function () {
				getFile("run?cmd=select station " + stId, function () {
					getFile("run?cmd=attack -d -t 30s", function () {
						showMessage("Deauth 30s для станции #" + stId);
						setScanStatus("Deauth запущен — соединение с ESP может оборваться");
					});
				});
			});
		});
	});
}

function updateWorkflow() {
	var steps = ["wf1", "wf2", "wf3", "wf4", "wf5", "wf6"];
	var i;
	for (i = 0; i < steps.length; i++) {
		if (!getE(steps[i])) continue;
		getE(steps[i]).className = "step";
	}
	var apCount = scanJson.aps.length;
	var sel = getSelectedApIndices();
	var stCount = scanJson.stations.length;
	var selSt = 0;
	for (i = 0; i < scanJson.stations.length; i++) {
		if (scanJson.stations[i][scanJson.stations[i].length - 1]) selSt++;
	}

	if (getE("wf1")) getE("wf1").className = apCount > 0 ? "step done" : "step active";
	if (getE("wf2")) getE("wf2").className = sel.length > 0 ? "step done" : (apCount > 0 ? "step active" : "step");
	if (getE("wf3")) getE("wf3").className = stCount > 0 ? "step done" : (sel.length > 0 ? "step active" : "step");
	if (getE("wf4")) getE("wf4").className = stCount > 0 ? "step done" : "step";
	if (getE("wf5")) getE("wf5").className = (stCount > 0 && getFilterApIndex() >= 0) ? "step done" : "step";
	if (getE("wf6")) getE("wf6").className = selSt > 0 ? "step active" : "step";
}

function drawScan() {
	var html;
	var selected;
	var apName;
	var lock;
	var selectLabel;
	var apIdx;
	var clientCount;
	normalizeFilterApIndex();
	var filterAp = getFilterApIndex();
	var visibleStations = 0;

	// Access Points
	getE("apNum").innerHTML = scanJson.aps.length;
	html = "";

	for (var i = 0; i < scanJson.aps.length; i++) {
		selected = scanJson.aps[i][scanJson.aps[i].length - 1];
		selectLabel = selected
			? "<span class='desktop-only'>Снять</span><span class='mobile-only'>Снять</span>"
			: "<span class='desktop-only'>Выбрать</span><span class='mobile-only'>Выбрать</span>";
		selectBtnClass = selected ? "orange compact-button" : "green compact-button";
		var apDev = apDeviceLabel(scanJson.aps[i]);
		apName = scanJson.aps[i][1].length > 0
			? esc(scanJson.aps[i][1])
			: esc(scanJson.aps[i][0]);
		lock = scanJson.aps[i][4] == "-" ? "" : "&#x1f512;";
		clientCount = countClientsForAp(i);
		var sec = getSecurityHints(scanJson.aps[i][4]);

		html += "<div class='card" + (selected ? " selected" : "") + "'>"
			+ "<div class='card-header'>"
			+ "<span class='scan-title scan-ellipsis' title='" + esc(scanJson.aps[i][5]) + "'>#" + i + " " + esc(scanJson.aps[i][0]) + "</span>"
			+ "<span class='scan-side scan-ellipsis' title='" + esc(apDev) + "'>" + esc(apDev) + "</span>"
			+ "<span class='scan-lock'>" + lock + "</span>"
			+ "</div>"
			+ "<div class='select-btns' style='margin-bottom:0.5rem'>"
			+ "<button class='green compact-button' style='width:100%' onclick='saveApToEeprom(" + i + ")'>"
			+ "<span class='desktop-only'>Сохранить в EEPROM</span><span class='mobile-only'>В EEPROM</span>"
			+ "</button></div>"
			+ "<div class='card-body'>"
			+ "<div style='display:flex; gap:0.5rem; align-items:flex-start;'>"
			+ "<div style='flex:1;'>"
			+ "<p><strong>Название:</strong> <span class='scan-ellipsis' title='" + (scanJson.aps[i][1].length > 0 ? esc(scanJson.aps[i][1]) : "") + "'>" + apName + "</span></p>"
			+ "<p><strong>Канал:</strong> " + esc(scanJson.aps[i][2]) + "</p>"
			+ "<p><strong>Сигнал:</strong> " + renderSignal(scanJson.aps[i][3]) + "</p>"
			+ "</div>"
			+ "<div class='signal-gauge'>" + rssiGaugeSvg(scanJson.aps[i][3]) + "</div>"
			+ "</div>"
			+ "<p><strong>Шифрование:</strong> <span class='scan-ellipsis' title='" + esc(scanJson.aps[i][4]) + "'>" + esc(scanJson.aps[i][4]) + "</span></p>"
			+ "<p><strong>MAC:</strong> <span class='scan-mono scan-ellipsis' title='" + esc(scanJson.aps[i][5]) + "'>" + esc(scanJson.aps[i][5]) + "</span></p>"
			+ "<p><strong>По MAC (OUI):</strong> <span class='scan-ellipsis' title='" + esc(apDev) + "'>" + esc(apDev) + "</span></p>"
			+ "<p><strong>Клиентов:</strong> " + clientCount + "</p>"
			+ "<p class='sec-hint'><strong>WPS:</strong> " + sec.wps + " · <strong>PMF:</strong> " + sec.pmf + "</p>"
			+ "</div>"
			+ "<div class='select-btns'>"
			+ "<button class='" + selectBtnClass + "' onclick='selectRow(0," + i + "," + (selected ? "false" : "true") + ")'>" + selectLabel + "</button>"
			+ "<button class='cyan compact-button' onclick='scanClientsForAp(" + i + ")'><span class='desktop-only'>Клиенты</span><span class='mobile-only'>Клиент.</span></button>"
			+ "<button class='compact-button purple' title='Эксперимент: deauth + захват EAPOL (только своя AP!)' onclick='captureHandshake(" + i + ",-1)'><span class='desktop-only'>HS</span><span class='mobile-only'>HS</span></button>"
			+ "<button class='red' onclick='remove(0," + i + ")'>Удалить</button>"
			+ "</div>"
			+ "</div>";
	}

	if (scanJson.aps.length === 0) {
		html = "<div class='card'><div class='card-body'>Нет точек доступа — нажмите «Скан Wi-Fi Сети»</div></div>";
	}

	getE("apTable").innerHTML = html;

	// Stations
	html = "";

	for (var i = 0; i < scanJson.stations.length; i++) {
		apIdx = getStationApIndex(scanJson.stations[i]);
		if (apIdx < 0) continue;
		if (filterAp >= 0 && apIdx !== filterAp) continue;

		visibleStations++;
		selected = scanJson.stations[i][scanJson.stations[i].length - 1];
		selectLabel = selected
			? "<span class='desktop-only'>Снять</span><span class='mobile-only'>Снять</span>"
			: "<span class='desktop-only'>Выбрать</span><span class='mobile-only'>Выбрать</span>";
		var ap = "";
		if (apIdx >= 0 && scanJson.aps[apIdx])
			ap = esc(scanJson.aps[apIdx][0]);

		var online = getOnlineStatus(scanJson.stations[i][6]);
		var st = scanJson.stations[i];
		var devLabel = stationDeviceLabel(st);
		var userName = savedDeviceName(st[0]);
		if (!userName && st[2] && st[2].length > 0 && st[2].indexOf("device_") !== 0) userName = st[2];

		html += "<div class='card" + (selected ? " selected" : "") + "'>"
			+ "<div class='card-header'>"
			+ "<span class='scan-title scan-ellipsis' title='" + esc(st[0]) + "'>#" + i + " " + esc(devLabel) + "</span>"
			+ "<span class='scan-side'>Ch " + esc(st[1]) + "</span>"
			+ "</div>"
			+ "<div class='card-body'>"
			+ "<p><strong>MAC:</strong> <span class='scan-mono scan-ellipsis' title='" + esc(st[0]) + "'>" + esc(st[0]) + "</span></p>"
			+ "<p><strong>По MAC (OUI):</strong> <span class='scan-ellipsis' title='" + esc(devLabel) + "'>" + esc(devLabel) + "</span></p>"
			+ "<p><strong>Метка:</strong> <span class='scan-ellipsis' title='" + (userName ? esc(userName) : "") + "'>" + (userName ? esc(userName) : "<button onclick='add(1," + i + ")'>" + lang("add") + "</button>") + "</span></p>"
			+ "<p><strong>Пакеты:</strong> " + esc(scanJson.stations[i][4]) + "</p>"
			+ "<p><strong>АП:</strong> <span class='scan-ellipsis' title='" + ap + "'>" + ap + "</span></p>"
			+ "<p><strong>Активность:</strong> <span class='badge-online " + online.cls + "'>" + online.label + "</span> · " + esc(scanJson.stations[i][6]) + "</p>"
			+ "</div>"
			+ "<div class='select-btns'>"
			+ "<button class='" + selectBtnClass + "' onclick='selectRow(1," + i + "," + (selected ? "false" : "true") + ")'>" + selectLabel + "</button>"
			+ "<button class='compact-button purple' onclick='captureHandshakeForStation(" + i + ")' title='Захват handshake с этим клиентом'><span class='desktop-only'>HS</span><span class='mobile-only'>HS</span></button>"
			+ "<button class='red compact-button' onclick='deauthStation(" + i + ")'><span class='desktop-only'>Deauth</span><span class='mobile-only'>Deauth</span></button>"
			+ "<button class='red' onclick='remove(1," + i + ")'>Удалить</button>"
			+ "</div>"
			+ "</div>";
	}

	getE("stNum").innerHTML = visibleStations;

	if (getE("stFilterHint")) {
		if (filterAp >= 0 && scanJson.aps[filterAp]) {
			var apLabel = scanJson.aps[filterAp][1].length > 0 ? scanJson.aps[filterAp][1] : scanJson.aps[filterAp][0];
			getE("stFilterHint").innerHTML = "(фильтр: " + esc(apLabel) + ", всего " + scanJson.stations.length + ")";
		} else {
			getE("stFilterHint").innerHTML = scanJson.stations.length > 0 ? "(все " + scanJson.stations.length + ")" : "";
		}
	}

	if (visibleStations === 0) {
		if (scanJson.stations.length === 0) {
			html = "<div class='card'><div class='card-body'>Нет станций — выберите AP и нажмите «Скан клиентов»</div></div>";
		} else if (filterAp >= 0) {
			html = "<div class='card'><div class='card-body'>Нет клиентов у выбранной AP. Увеличьте время скана или нажмите «Показать всех».</div></div>";
		}
	}

	getE("stTable").innerHTML = html;
	drawChannelMap();
	drawRssiChart();
	drawScanCones();
	updateWorkflow();
}

function drawNames() {
	var html;
	var selected;
	var selectLabel;
	var selectBtnClass;

	getE("nNum").innerHTML = nameJson.length;
	html = "";

	for (var i = 0; i < nameJson.length; i++) {
		selected = nameJson[i][nameJson[i].length - 1];
		selectLabel = selected
			? "<span class='desktop-only'>Снять</span><span class='mobile-only'>Снять</span>"
			: "<span class='desktop-only'>Выбрать</span><span class='mobile-only'>Выбрать</span>";
		selectBtnClass = selected ? "orange compact-button" : "green compact-button";

		html += "<div class='card" + (selected ? " selected" : "") + "'>"
			+ "<div class='card-header'><span class='scan-title scan-ellipsis' title='" + esc(nameJson[i][2]) + "'>#" + i + " " + esc(nameJson[i][2].substring(0, 16)) + "</span><span class='scan-side scan-ellipsis' title='" + esc(nameJson[i][1]) + "'>" + esc(nameJson[i][1]) + "</span></div>"
			+ "<div class='card-body'>"
			+ "<p><strong>MAC:</strong> <input class='scan-mono-input' type='text' id='name_" + i + "_mac' value='" + esc(nameJson[i][0]) + "'></p>"
			+ "<p><strong>Название:</strong> <input type='text' id='name_" + i + "_name' value='" + esc(nameJson[i][2].substring(0, 16)) + "'></p>"
			+ "<p><strong>AP-BSSID:</strong> <input class='scan-mono-input' type='text' id='name_" + i + "_apbssid' value='" + esc(nameJson[i][3]) + "'></p>"
			+ "<p><strong>Канал:</strong> <input type='number' id='name_" + i + "_ch' value='" + esc(nameJson[i][4]) + "'></p>"
			+ "</div>"
			+ "<div class='select-btns'>"
			+ "<button class='green compact-button' onclick='save(" + i + ")'><span class='desktop-only'>" + lang("save") + "</span><span class='mobile-only'>ОК</span></button>"
			+ "<button class='compact-button' onclick='selectRow(2," + i + "," + (selected ? "false" : "true") + ")'>" + selectLabel + "</button>"
			+ "<button class='red' onclick='remove(2," + i + ")'>Удалить</button>"
			+ "</div>"
			+ "</div>";
	}

	if (nameJson.length === 0) {
		html = "<div class='card'><div class='card-body'>Нет сохранённых устройств</div></div>";
	}

	getE("nTable").innerHTML = html;
}

var duts;
var elxtime;

function buildScanCmd(type) {
	var chSel = getE("ch").options[getE("ch").selectedIndex].value;

	if (type === 1) {
		var sel = getSelectedApIndices();
		var scanSec = parseInt(getE("scanTime").value, 10) || 30;
		if (scanSec < 15) scanSec = 15;
		if (scanSec > 45) scanSec = 45;
		getE("scanTime").value = scanSec;
		if (sel.length === 1) chSel = String(scanJson.aps[sel[0]][2]);
		return "scan stations -t " + scanSec + "s -ch " + chSel;
	}

	if (type === 0 && chSel !== "all") {
		return "scan aps -ch " + chSel;
	}
	return "scan aps ";
}

function ensureBusyOverlay() {
	var el = document.getElementById("busyOverlay");
	if (el) return el;

	el = document.createElement("div");
	el.id = "busyOverlay";
	el.className = "busy-overlay";
	el.innerHTML = "<div class='busy-overlay-box'>"
		+ "<div class='busy-spinner'></div>"
		+ "<h3 id='busyOverlayTitle'>Подождите</h3>"
		+ "<p id='busyOverlayText'></p>"
		+ "<p class='busy-time' id='busyOverlayTime'></p>"
		+ "</div>";
	document.body.appendChild(el);

	if (!document.getElementById("busyOverlayStyle")) {
		var st = document.createElement("style");
		st.id = "busyOverlayStyle";
		st.textContent = ".busy-overlay{display:none;position:fixed;inset:0;z-index:10000;background:rgba(0,0,0,.78);align-items:center;justify-content:center;padding:1rem;box-sizing:border-box}"
			+ ".busy-overlay.open{display:flex}"
			+ ".busy-overlay-box{max-width:320px;width:100%;text-align:center;background:rgba(20,20,45,.96);border:1px solid rgba(0,212,255,.3);border-radius:12px;padding:1.5rem 1.25rem;box-shadow:0 0 40px rgba(0,212,255,.12)}"
			+ ".busy-overlay-box h3{font-family:Orbitron,sans-serif;font-size:.95rem;color:#00d4ff;margin:.9rem 0 .5rem}"
			+ ".busy-overlay-box p{font-size:.78rem;line-height:1.45;color:#e0e0ff}"
			+ ".busy-time{color:#00ff9d;font-size:.72rem;margin-top:.65rem}"
			+ ".busy-spinner{width:42px;height:42px;margin:0 auto;border:3px solid rgba(0,212,255,.2);border-top-color:#00d4ff;border-radius:50%;animation:busySpin .85s linear infinite}"
			+ "@keyframes busySpin{to{transform:rotate(360deg)}}";
		document.head.appendChild(st);
	}
	return el;
}

function showBusyOverlay(cfg) {
	var modal = ensureBusyOverlay();
	document.getElementById("busyOverlayTitle").textContent = cfg.title || "Подождите";
	document.getElementById("busyOverlayText").innerHTML = cfg.html || esc(cfg.text || "Выполняется операция…");
	var timeEl = document.getElementById("busyOverlayTime");
	timeEl.textContent = "";
	modal.classList.add("open");
}

function hideBusyOverlay() {
	var el = document.getElementById("busyOverlay");
	if (el) el.classList.remove("open");
}

function runScanCmd(type, elxtime) {
	getE('RButton').disabled = true;
	if (type === 0) {
		getE('scanZero').disabled = true;
	}
	duts = type;
	setTimeout(buttonFunc, elxtime);
	if (type === 1) {
		showBusyOverlay({
			title: "Скан клиентов",
			sec: Math.round(elxtime / 1000),
			html: "Глубокий скан клиентов… радио на канале цели.<br>На время скана SoftAP/веб могут отключиться — дождитесь окончания, переподключитесь и нажмите «Обновить»."
		});
		pollScanResults(elxtime + 500, 0);
	} else {
		setTimeout(function () { load(true); }, elxtime + 500);
	}
}

function setChannelDropdown(ch) {
	var chEl = getE("ch");
	if (!chEl) return;
	var i;
	for (i = 0; i < chEl.options.length; i++) {
		if (chEl.options[i].value === String(ch)) {
			chEl.selectedIndex = i;
			return;
		}
	}
}

function pollScanResults(delayMs, attempt) {
	setTimeout(function () {
		getFile("run?cmd=save scan", function () {
			getFile("scan.json", function (res) {
				var data = JSON.parse(res);
				var st = data.stations ? data.stations.length : 0;
				var pkts = data.sniff_pkts || 0;
				scanJson = data;
				sanitizeScanJson();
				drawScan();

				if (st === 0 && attempt < 8) {
					setScanStatus("Скан… пакетов " + pkts + ", клиентов " + st + " — подождите или «Обновить» (" + (attempt + 2) + "/9)");
					document.getElementById("busyOverlayText").innerHTML = "Скан клиентов… попытка " + (attempt + 2) + "/9<br>Пакетов: " + pkts;
					pollScanResults(4000, attempt + 1);
					return;
				}

				showMessage("connected");
				if (st > 0) {
					setScanStatus("Клиентов: " + st + ", пакетов в эфире: " + pkts);
				} else if (pkts < 5) {
					setScanStatus("Нет пакетов (" + pkts + ") — прошивка с sniff_pkts? Канал AP, 30–45 с, телефон/ПК на Wi‑Fi цели");
				} else {
					setScanStatus("Пакеты есть (" + pkts + "), клиентов 0 — PMF/WPA3 или другая AP на канале");
				}
				getE('RButton').disabled = false;
				hideBusyOverlay();
			}, 15000, "GET", function () {
				if (attempt < 8) pollScanResults(4000, attempt + 1);
				else hideBusyOverlay();
			}, function () {
				hideBusyOverlay();
			});
		}, 15000, "GET", function () {
			if (attempt < 8) pollScanResults(3000, attempt + 1);
			else hideBusyOverlay();
		}, function () {
			hideBusyOverlay();
		});
	}, delayMs);
}

function getStationScanSeconds() {
	var sec = parseInt(getE("scanTime").value, 10) || 30;
	if (sec < 15) sec = 15;
	if (sec > 45) sec = 45;
	getE("scanTime").value = sec;
	return sec;
}

function ensureStationScanModal() {
	var el = document.getElementById("stScanModal");
	if (el) return el;

	el = document.createElement("div");
	el.id = "stScanModal";
	el.className = "st-scan-modal";
	el.innerHTML = "<div class='st-scan-modal-box' role='dialog' aria-modal='true' aria-labelledby='stScanModalTitle'>"
		+ "<h3 id='stScanModalTitle'></h3>"
		+ "<div id='stScanModalText'></div>"
		+ "<div class='st-scan-modal-btns'>"
		+ "<button type='button' class='st-scan-modal-cancel'>Отмена</button>"
		+ "<button type='button' class='st-scan-modal-ok green'>Подтвердить</button>"
		+ "</div></div>";
	document.body.appendChild(el);

	if (!document.getElementById("stScanModalStyle")) {
		var st = document.createElement("style");
		st.id = "stScanModalStyle";
		st.textContent = ".st-scan-modal{display:none;position:fixed;inset:0;z-index:9999;background:rgba(0,0,0,.72);align-items:center;justify-content:center;padding:1rem;box-sizing:border-box}"
			+ ".st-scan-modal.open{display:flex}"
			+ ".st-scan-modal-box{max-width:380px;width:100%;background:rgba(20,20,45,.95);border:1px solid rgba(100,200,255,.25);border-radius:12px;padding:1.25rem;box-shadow:0 0 30px rgba(0,212,255,.15)}"
			+ ".st-scan-modal-box h3{font-family:Orbitron,sans-serif;font-size:1rem;color:#00d4ff;margin-bottom:.75rem}"
			+ "#stScanModalText{font-size:.8rem;line-height:1.5;color:#e0e0ff;margin-bottom:1.25rem}"
			+ ".modal-time-report{background:rgba(0,212,255,.08);border:1px solid rgba(0,212,255,.25);border-radius:8px;padding:.75rem 1rem;margin-bottom:.85rem;text-align:center}"
			+ ".modal-time-report .time-val{font-family:Orbitron,sans-serif;font-size:1.35rem;color:#00ff9d;display:block;margin-top:.25rem}"
			+ ".modal-details{margin-top:.65rem;color:#a0a0cc;font-size:.75rem}"
			+ ".modal-warn{color:#ffaa00;font-size:.72rem;margin-top:.5rem}"
			+ ".st-scan-modal-btns{display:flex;gap:.75rem;justify-content:flex-end;flex-wrap:wrap}"
			+ ".st-scan-modal-btns button{flex:1;min-width:120px;padding:.65rem 1rem;font-family:inherit;font-size:.8rem;border-radius:8px;cursor:pointer;border:none;font-weight:700;transition:all .2s ease}"
			+ ".st-scan-modal-btns .st-scan-modal-cancel{background:linear-gradient(135deg,#ff3366,#c02040);color:#fff;border-color:rgba(255,51,102,.55)}"
			+ ".st-scan-modal-btns .st-scan-modal-cancel:hover{box-shadow:0 0 20px rgba(255,51,102,.5)}"
			+ ".st-scan-modal-btns .st-scan-modal-ok{background:linear-gradient(135deg,#00ff9d,#00c878);color:#0f0f1a;border-color:rgba(0,255,157,.55)}"
			+ ".st-scan-modal-btns .st-scan-modal-ok:hover{box-shadow:0 0 20px rgba(0,255,157,.5)}";
		document.head.appendChild(st);
	}
	return el;
}

function showReconnectModal(cfg) {
	var modal = ensureStationScanModal();
	var titleEl = document.getElementById("stScanModalTitle");
	var text = document.getElementById("stScanModalText");
	var okBtn = modal.querySelector(".st-scan-modal-ok");
	var cancelBtn = modal.querySelector(".st-scan-modal-cancel");
	var sec = cfg.sec || 30;

	titleEl.textContent = cfg.title || "Подтверждение";

	var html = "<div class='modal-time-report'>Время операции<span class='time-val'>~" + sec + " с</span></div>";

	if (cfg.apOffDuringCapture) {
		html += "<p>На <strong>~" + sec + " с</strong> Wi‑Fi ESP <strong>выключится</strong> (захват на канале цели).</p>"
			+ "<p>Затем снова появится <strong>cicada3301</strong> — подключитесь, нажмите <strong>«Проверить HS»</strong> или дождитесь сообщения внизу.</p>"
			+ "<p>Надёжнее: кнопка <strong>HS</strong> в строке <strong>клиента</strong>, а не только у AP.</p>";
	} else if (cfg.keepApConnected) {
		html += "<p>Точка ESP <strong>остаётся</strong>, канал = каналу цели. После — <strong>«Обновить»</strong>.</p>";
	} else {
		html += "<p>После операции нажмите <strong>«Обновить»</strong>.</p>";
	}

	html += "<div class='modal-details'>";
	if (cfg.apName) html += "<div><strong>AP:</strong> " + esc(cfg.apName) + "</div>";
	if (cfg.channel !== undefined && cfg.channel !== "") html += "<div><strong>Канал:</strong> " + esc(String(cfg.channel)) + "</div>";
	if (cfg.clientLabel) html += "<div><strong>" + esc(cfg.clientLabel) + "</strong></div>";
	if (cfg.clientMac) html += "<div><strong>MAC:</strong> <span style='font-size:.7rem'>" + esc(cfg.clientMac) + "</span></div>";
	html += "</div>";

	if (cfg.warning) html += "<p class='modal-warn'>" + esc(cfg.warning) + "</p>";

	text.innerHTML = html;

	function close() {
		modal.classList.remove("open");
		okBtn.onclick = null;
		cancelBtn.onclick = null;
		modal.onclick = null;
	}

	okBtn.onclick = function () {
		close();
		if (typeof cfg.onConfirm === "function") cfg.onConfirm();
	};
	cancelBtn.onclick = close;
	modal.onclick = function (e) {
		if (e.target === modal) close();
	};

	modal.classList.add("open");
}

function confirmStationScan(sec, apName, onConfirm) {
	showReconnectModal({
		title: "Скан станций (клиентов)",
		sec: sec,
		apName: apName,
		keepApConnected: true,
		onConfirm: onConfirm
	});
}

function runStationScan() {
	var sec = getStationScanSeconds();
	var sel = getSelectedApIndices();
	var apName = sel.length === 1 && scanJson.aps[sel[0]] ? scanJson.aps[sel[0]][0] : "";

	getE('RButton').disabled = true;
	getE('scanZero').disabled = true;
	getE('scanOne').style.visibility = 'hidden';
	elxtime = sec * 1000 + 12000;
	setScanStatus("Скан клиентов ~" + sec + " с, канал " + scanJson.aps[sel[0]][2] + " — AP ESP включён, по окончании «Обновить»");
	startStationScanOnDevice(function () {
		runScanCmd(1, elxtime);
	});
}

function startStationScanOnDevice(done) {
	var sel = getSelectedApIndices();
	if (sel.length !== 1) {
		getFile("run?cmd=" + buildScanCmd(1), done);
		return;
	}

	var apIdx = sel[0];
	var ch = scanJson.aps[apIdx][2];
	setChannelDropdown(ch);

	getFile("run?cmd=deselect ap", function () {
		getFile("run?cmd=select ap " + apIdx, function () {
			getFile("run?cmd=set channel " + ch, function () {
				getFile("run?cmd=" + buildScanCmd(1), done);
			});
		});
	});
}

function scan(type) {
	if (type === 1) {
		if (scanJson.aps.length < 1) {
			showMessage("Сначала: Скан Wi-Fi Сети");
			setScanStatus("Нужен список AP (шаг 1)");
			return;
		}
		var selected = getSelectedApIndices();
		if (selected.length < 1) {
			showMessage("Выберите одну AP");
			setScanStatus("Выберите целевую AP (шаг 2)");
			return;
		}
		if (selected.length > 1) {
			showMessage("Выберите только одну AP");
			setScanStatus("Снимите лишние AP — нужна одна цель");
			return;
		}
		filterApIndex = selected[0];
		if (getE("filterByAp")) getE("filterByAp").checked = true;

		var sec = getStationScanSeconds();
		var ap = scanJson.aps[selected[0]];
		showReconnectModal({
			title: "Скан станций (клиентов)",
			sec: sec,
			apName: ap[0],
			channel: ap[2],
			keepApConnected: true,
			onConfirm: runStationScan
		});
		return;
	}

	getE('RButton').disabled = true;

	switch (type) {
		case 0:
			getE('scanOne').disabled = true;
			getE('scanZero').style.visibility = 'hidden';
			elxtime = 15000;
			setScanStatus("Скан Wi-Fi Сети…");
			showBusyOverlay({
				title: "Скан Wi-Fi Сети",
				sec: 15,
				html: ""
			});
			getFile("run?cmd=" + buildScanCmd(0));
			runScanCmd(0, elxtime);
			return;
	}

	getFile("run?cmd=" + buildScanCmd(type));
	runScanCmd(type, elxtime);
}

function buttonFunc() {
	switch (duts) {
		case 0:
			getE('scanZero').style.visibility = 'visible';
			getE('scanZero').disabled = false;
			getE('scanOne').disabled = false;
			setScanStatus("Скан Wi-Fi Сети завершён — нажмите «Обновить»");
			break;
		case 1:
			getE('scanOne').style.visibility = 'visible';
			getE('scanZero').disabled = false;
			setScanStatus("Скан клиентов завершён — нажмите «Обновить»");
			break;
	}
	getE('RButton').disabled = false;
}

function load(showOverlay) {
	var pending = 2;
	var waitUi = showOverlay === true;

	function loadDone() {
		pending--;
		if (pending <= 0) {
			if (waitUi) hideBusyOverlay();
			if (waitUi) getE('RButton').disabled = false;
			checkHandshakeStatus();
		}
	}

	if (waitUi) {
		var busyTitle = document.getElementById("busyOverlay");
		if (busyTitle && busyTitle.classList.contains("open")) {
			document.getElementById("busyOverlayTitle").textContent = "Обновление";
			document.getElementById("busyOverlayText").innerHTML = "Загрузка …";
			document.getElementById("busyOverlayTime").textContent = "";
		} else {
			showBusyOverlay({
				title: "Обновление",
				html: "Загрузка …<br>Подождите."
			});
		}
		getE('RButton').disabled = true;
	}

	getFile("run?cmd=save scan", function () {
		getFile("scan.json", function (res) {
			scanJson = JSON.parse(res);
			sanitizeScanJson();
			showMessage("connected");
			drawScan();
			if (scanJson.aps.length === 0 && scanJson.stations.length === 0) {
				setScanStatus("«Скан Wi-Fi Сети» → «Обновить» — клиенты появятся в фоне; для глубокого скана: выберите AP → «Скан клиентов»");
			} else if (scanJson.stations.length === 0 && scanJson.aps.length > 0) {
				setScanStatus("AP: " + scanJson.aps.length + " — клиенты в фоне, подождите 10–20 с и «Обновить»; или «Скан клиентов» для выбранной AP");
			} else {
				setScanStatus("AP: " + scanJson.aps.length + ", клиентов: " + scanJson.stations.length + " — фоновое обновление ~2 с");
			}
			loadDone();
		}, 15000, "GET", function () {
			loadDone();
			setScanStatus("Таймаут scan.json — проверьте Wi‑Fi к ESP и «Обновить» снова");
		}, function () {
			loadDone();
		});
	}, 15000, "GET", function () {
		loadDone();
	}, function () {
		loadDone();
	});

	getFile("run?cmd=save names", function () {
		getFile("names.json", function (res) {
			nameJson = JSON.parse(res);
			drawNames();
			loadDone();
		}, 15000, "GET", function () {
			loadDone();
		}, function () {
			loadDone();
		});
	}, 15000, "GET", function () {
		loadDone();
	}, function () {
		loadDone();
	});
}

function selectRow(type, id, selected) {
	var j;
	switch (type) {
		case 0:
			if (selected) {
				for (j = 0; j < scanJson.aps.length; j++) {
					if (j !== id && scanJson.aps[j][scanJson.aps[j].length - 1]) {
						scanJson.aps[j][scanJson.aps[j].length - 1] = false;
						getFile("run?cmd=deselect ap " + j);
					}
				}
				filterApIndex = id;
				if (getE("filterByAp")) getE("filterByAp").checked = true;
			} else if (filterApIndex === id) {
				filterApIndex = -1;
			}
			scanJson.aps[id][scanJson.aps[id].length - 1] = selected;
			drawScan();
			getFile("run?cmd=" + (selected ? "" : "de") + "select ap " + id);
			if (selected) {
				setChannelDropdown(scanJson.aps[id][2]);
				setScanStatus("AP #" + id + " — канал " + scanJson.aps[id][2] + ", «Скан Клиентов» 30–45 с");
			}
			break;
		case 1:
			if (selected) {
				for (j = 0; j < scanJson.stations.length; j++) {
					if (j !== id && scanJson.stations[j][scanJson.stations[j].length - 1]) {
						scanJson.stations[j][scanJson.stations[j].length - 1] = false;
						getFile("run?cmd=deselect station " + j);
					}
				}
			}
			scanJson.stations[id][scanJson.stations[id].length - 1] = selected;
			drawScan();
			getFile("run?cmd=" + (selected ? "" : "de") + "select station " + id);
			if (selected) {
				setScanStatus("Клиент выбран — откройте «Атаки» → Deauth (только своя AP!)");
			}
			break;
		case 2:
			save(id);
			nameJson[id][5] = selected;
			drawNames();
			getFile("run?cmd=" + (selected ? "" : "de") + "select name " + id);
	}
}

function remove(type, id) {
	switch (type) {
		case 0:
			scanJson.aps.splice(id, 1);
			if (filterApIndex === id) filterApIndex = -1;
			else if (filterApIndex > id) filterApIndex--;
			drawScan();
			getFile("run?cmd=remove ap " + id);
			break;
		case 1:
			scanJson.stations.splice(id, 1);
			drawScan();
			getFile("run?cmd=remove station " + id);
			break;
		case 2:
			nameJson.splice(id, 1);
			drawNames();
			getFile("run?cmd=remove name " + id);
	}
}

function save(id) {
	var mac = (getE("name_" + id + "_mac").value || getE("name_" + id + "_mac").innerHTML || "").replace("<br>", "");
	var name = (getE("name_" + id + "_name").value || getE("name_" + id + "_name").innerHTML || "").replace("<br>", "");
	var apbssid = (getE("name_" + id + "_apbssid").value || getE("name_" + id + "_apbssid").innerHTML || "").replace("<br>", "");
	var ch = (getE("name_" + id + "_ch").value || getE("name_" + id + "_ch").innerHTML || "").replace("<br>", "");
	var changed = mac != nameJson[id][0] || name != nameJson[id][2] || apbssid != nameJson[id][3] || ch != nameJson[id][4];
	if (changed) {
		nameJson[id][0] = mac;
		nameJson[id][2] = name;
		nameJson[id][3] = apbssid;
		nameJson[id][4] = ch;

		if (nameJson[id][0].length != 17) {
			showMessage("ERROR: MAC invalid");
			return;
		}

		getFile("run?cmd=replace name " + id + " -n \"" + nameJson[id][2] + "\" -m \"" + nameJson[id][0] + "\" -ch " + nameJson[id][4] + " -b \"" + nameJson[id][3] + "\" " + (nameJson[id][5] ? "-s" : ""));

		drawNames();
	}
}

function add(type, id) {
	if (nameJson.length >= 25) {
		showMessage("Device Name List is full!");
		return;
	}

	switch (type) {
		case 0: {
			var apRow = scanJson.aps[id];
			var apLabel = apDeviceLabel(apRow);
			getFile("run?cmd=add name \"" + quoteCmd(apLabel) + "\" -ap " + id);
			apRow[1] = apRow[1].length > 0 ? apRow[1] : apLabel;
			nameJson.push([apRow[5], apRow[6] || apLabel, apRow[1], "", apRow[2], false]);
			drawScan();
			break;
		}
		case 1: {
			var stRow = scanJson.stations[id];
			var autoLabel = stationDeviceLabel(stRow);
			if (autoLabel === "Неизвестно" || autoLabel === "Приватный MAC") {
				autoLabel = "device_" + nameJson.length;
			}
			var apMac = "";
			var stApIdx = getStationApIndex(stRow);
			if (stApIdx >= 0 && scanJson.aps[stApIdx]) {
				apMac = scanJson.aps[stApIdx][5];
			}
			getFile(
				"run?cmd=add name \"" + quoteCmd(autoLabel) + "\" -m \"" + stRow[0] + "\" -ch " + stRow[1] +
				(apMac ? " -b \"" + apMac + "\"" : "")
			);
			stRow[2] = autoLabel;
			nameJson.push([stRow[0], stRow[3] || autoLabel, autoLabel, apMac, stRow[1], false]);
			drawScan();
			break;
		}
		case 2:
			getFile("run?cmd=add name device_" + nameJson.length + " -m 00:00:00:00:00:00 -ch 1");
			nameJson.push(["00:00:00:00:00:00", "", "device_" + nameJson.length, "", 1, false]);
			drawNames();
	}

	drawNames();
}

function selectAll(type, select) {
	switch (type) {
		case 0:
			getFile("run?cmd=" + (select ? "" : "de") + "select aps");
			for (var i = 0; i < scanJson.aps.length; i++) scanJson.aps[i][7] = select;
			drawScan();
			break;
		case 1:
			getFile("run?cmd=" + (select ? "" : "de") + "select stations");
			for (var i = 0; i < scanJson.stations.length; i++) scanJson.stations[i][7] = select;
			drawScan();
			break;
		case 2:
			getFile("run?cmd=" + (select ? "" : "de") + "select names");
			for (var i = 0; i < nameJson.length; i++) nameJson[i][5] = select;
			drawNames();
	}
}

if (getE("filterByAp")) {
	getE("filterByAp").addEventListener("change", function () {
		if (this.checked && filterApIndex < 0) {
			var sel = getSelectedApIndices();
			if (sel.length === 1) filterApIndex = sel[0];
		}
		drawScan();
	});
}

(function scanConesLoop() {
	function tick() {
		if (getE("scanCones")) drawScanCones();
		requestAnimationFrame(tick);
	}
	if (document.readyState === "loading") {
		document.addEventListener("DOMContentLoaded", tick);
	} else {
		tick();
	}
	window.addEventListener("resize", function () {
		if (getE("scanCones")) drawScanCones();
	});
})();
