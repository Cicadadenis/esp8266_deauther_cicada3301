/* WiFi radar map — AP position by channel (angle) and RSSI (distance) */

var scanJson = { aps: [], stations: [] };
var radarAutoTimer = null;
var selectedApIdx = -1;

var CX = 200;
var CY = 200;
var R_MAX = 175;

function load() {
	reloadScanJson(true);
	setupAutoRefresh();
}

function setupAutoRefresh() {
	var cb = document.getElementById("autoRefresh");
	if (!cb) return;

	function tick() {
		if (cb.checked) reloadScanJson(true);
	}

	if (radarAutoTimer) clearInterval(radarAutoTimer);
	radarAutoTimer = setInterval(tick, 5000);
}

function rssiToRadius(rssi) {
	var n = parseInt(rssi, 10);
	if (isNaN(n)) n = -80;
	if (n > -30) n = -30;
	if (n < -95) n = -95;
	var t = (n + 95) / 65;
	return R_MAX * (1 - t * 0.82) + 18;
}

function rssiToColor(rssi) {
	var n = parseInt(rssi, 10);
	if (isNaN(n)) return "#a0a0cc";
	if (n >= -55) return "#00ff88";
	if (n >= -70) return "#ffaa00";
	return "#ff2255";
}

function rssiToFilter(rssi) {
	var n = parseInt(rssi, 10);
	if (isNaN(n)) return "blipGlowOrange";
	if (n >= -55) return "blipGlowGreen";
	if (n >= -70) return "blipGlowOrange";
	return "blipGlowRed";
}

function channelToAngle(ch) {
	var c = parseInt(ch, 10);
	if (isNaN(c) || c < 1) c = 1;
	if (c > 14) c = 14;
	return ((c - 1) / 13) * Math.PI * 2 - Math.PI / 2;
}

function channelToMhz(ch) {
	var c = parseInt(ch, 10);
	if (isNaN(c) || c < 1) return 0;
	if (c === 14) return 2484;
	return 2412 + (c - 1) * 5;
}


function macHash(mac) {
	var m = (mac || "").toString();
	var h = 0;
	var i;
	for (i = 0; i < m.length; i++) h = ((h << 5) - h + m.charCodeAt(i)) | 0;
	return Math.abs(h);
}

function rssiToDistanceM(rssi) {
	var r = parseInt(rssi, 10);
	if (isNaN(r)) return "?";
	if (r >= -42) return 1;
	var d = Math.pow(10, (-40 - r) / 22);
	if (d < 1) d = 1;
	if (d > 150) d = 150;
	return Math.round(d);
}

function apDisplayName(ap) {
	var ssid = (ap[0] || "").trim();
	var name = (ap[1] || "").trim();
	if (!ssid || ssid === "*HIDDEN*" || ssid.indexOf("HIDDEN") >= 0) {
		return { text: name || "(Скрытый SSID)", hidden: true };
	}
	return { text: ssid, hidden: false };
}

function apBlipLabel(ap) {
	var ssid = (ap[0] || "").trim();
	if (!ssid || ssid === "*HIDDEN*" || ssid.indexOf("HIDDEN") >= 0) {
		ssid = (ap[1] || "").trim() || "(скрытый)";
	}
	if (ssid.length > 18) ssid = ssid.substring(0, 18) + "\u2026";
	return ssid;
}

function formatEnc(enc) {
	var e = (enc || "-").trim();
	if (e === "-" || e === "") return "[ESS]";
	return "[" + e + "]";
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

function sweepWedgePath() {
	var sweepDeg = 52;
	var a1 = -Math.PI / 2;
	var a2 = a1 + (sweepDeg * Math.PI / 180);
	var x1 = CX + Math.cos(a1) * R_MAX;
	var y1 = CY + Math.sin(a1) * R_MAX;
	var x2 = CX + Math.cos(a2) * R_MAX;
	var y2 = CY + Math.sin(a2) * R_MAX;
	return "M " + CX + " " + CY + " L " + x1.toFixed(1) + " " + y1.toFixed(1)
		+ " A " + R_MAX + " " + R_MAX + " 0 0 1 " + x2.toFixed(1) + " " + y2.toFixed(1) + " Z";
}

function drawRadarSweep() {
	var g = document.getElementById("radarSweep");
	if (!g) return;
	g.innerHTML = '<path class="radar-sweep-wedge" d="' + sweepWedgePath() + '"/>';
}

function drawRadarGrid() {
	var g = document.getElementById("radarGrid");
	if (!g) return;
	var html = "";
	var rings = [0.25, 0.5, 0.75, 1];
	var i;
	for (i = 0; i < rings.length; i++) {
		var r = R_MAX * rings[i];
		var cls = i === rings.length - 1 ? "radar-grid-outer" : "radar-grid";
		html += '<circle class="' + cls + '" cx="' + CX + '" cy="' + CY + '" r="' + r + '"/>';
	}
	for (i = 0; i < 12; i++) {
		var a = (i / 12) * Math.PI * 2 - Math.PI / 2;
		var x2 = CX + Math.cos(a) * R_MAX;
		var y2 = CY + Math.sin(a) * R_MAX;
		html += '<line class="radar-grid" x1="' + CX + '" y1="' + CY + '" x2="' + x2.toFixed(1) + '" y2="' + y2.toFixed(1) + '"/>';
	}
	var chLabels = [1, 7, 9, 10, 13, 14];
	for (i = 0; i < chLabels.length; i++) {
		var ang = channelToAngle(chLabels[i]);
		var lx = CX + Math.cos(ang) * (R_MAX + 16);
		var ly = CY + Math.sin(ang) * (R_MAX + 16);
		html += '<text class="radar-ch-label" x="' + lx.toFixed(1) + '" y="' + (ly + 4).toFixed(1)
			+ '" text-anchor="middle">ch' + chLabels[i] + "</text>";
	}

	g.innerHTML = html;
}

function toggleApList() {
	var panel = document.getElementById("apListPanel");
	var btn = document.getElementById("listToggle");
	if (!panel || !btn) return;
	var open = panel.classList.toggle("open");
	btn.classList.toggle("is-open", open);
	btn.textContent = open ? "▲ Скрыть список AP" : "▼ Список AP";
}

function countClientsForAp(apIdx) {
	var n = 0;
	var j;
	for (j = 0; j < scanJson.stations.length; j++) {
		if (getStationApIndex(scanJson.stations[j]) === apIdx) n++;
	}
	return n;
}

function getStationApIndex(st) {
	var ap = parseInt(st[5], 10);
	if (isNaN(ap) || ap < 0 || ap >= scanJson.aps.length) return -1;
	return ap;
}

function drawRadarBlips() {
	var g = document.getElementById("radarBlips");
	if (!g) return;
	var html = "";
	var i;
	var ap;
	var ang;
	var r;
	var x;
	var y;
	var clients;
	var sta;
	var apIdx;
	var apRssi;

	for (i = 0; i < scanJson.stations.length; i++) {
		sta = scanJson.stations[i];
		apIdx = getStationApIndex(sta);
		if (apIdx < 0) continue;
		apRssi = scanJson.aps[apIdx][3];
				var staSeed = macHash(sta[0]);
		ang = channelToAngle(scanJson.aps[apIdx][2]) + (((staSeed % 9) - 4) * 0.07);
		r = rssiToRadius(apRssi) * 0.72 + ((staSeed % 6) * 4);
		x = CX + Math.cos(ang) * r;
		y = CY + Math.sin(ang) * r;
		html += '<circle class="blip-sta" cx="' + x.toFixed(1) + '" cy="' + y.toFixed(1)
			+ '" r="3" fill="#c084fc" stroke="#ffffff" stroke-width="0.5" data-sta="' + i + '"/>';
	}

	for (i = 0; i < scanJson.aps.length; i++) {
		ap = scanJson.aps[i];
				var apSeed = macHash(ap[5]);
		ang = channelToAngle(ap[2]) + (((apSeed % 7) - 3) * 0.08);
		r = rssiToRadius(ap[3]);
		x = CX + Math.cos(ang) * r;
		y = CY + Math.sin(ang) * r;
		clients = countClientsForAp(i);
		var col = rssiToColor(ap[3]);
		var flt = rssiToFilter(ap[3]);
		var sel = i === selectedApIdx ? ' stroke="#ffffff" stroke-width="2.5"' : ' stroke="none"';
		var size = 7 + Math.min(clients, 4);
		html += '<g class="blip-ap" onclick="selectAp(' + i + ')" data-ap="' + i + '" filter="url(#' + flt + ')">';
		html += "<title>" + esc(apDisplayName(ap).text) + "</title>";
		html += '<circle cx="' + x.toFixed(1) + '" cy="' + y.toFixed(1) + '" r="' + (size + 8)
			+ '" fill="' + col + '" opacity="0.12"/>';
		html += '<circle cx="' + x.toFixed(1) + '" cy="' + y.toFixed(1) + '" r="' + (size + 4)
			+ '" fill="' + col + '" opacity="0.22"/>';
		html += '<circle class="blip-dot" cx="' + x.toFixed(1) + '" cy="' + y.toFixed(1) + '" r="' + size
			+ '" fill="' + col + '"' + sel + ' opacity="1"/>';
		html += '<circle cx="' + x.toFixed(1) + '" cy="' + y.toFixed(1) + '" r="' + (size + 2)
			+ '" fill="none" stroke="' + col + '" stroke-width="1.2" opacity="0.6"/>';
		html += '<text class="blip-ssid-label" x="' + x.toFixed(1) + '" y="' + (y - size - 10).toFixed(1)
			+ '" text-anchor="middle">' + esc(apBlipLabel(ap)) + "</text>";
		if (clients > 0) {
			html += '<text class="blip-clients-label" x="' + x.toFixed(1) + '" y="' + (y + size + 10).toFixed(1)
				+ '" text-anchor="middle">' + clients + "</text>";
		}
		html += "</g>";
	}

	g.innerHTML = html;
}

function drawApList() {
	var el = document.getElementById("apList");
	var empty = document.getElementById("emptyMsg");
	if (!el) return;

	if (!scanJson.aps || scanJson.aps.length === 0) {
		el.innerHTML = "";
		if (empty) empty.style.display = "block";
		return;
	}
	if (empty) empty.style.display = "none";

	var items = scanJson.aps.map(function (ap, idx) {
		return { ap: ap, idx: idx, rssi: parseInt(ap[3], 10) || -99 };
	});
	items.sort(function (a, b) { return b.rssi - a.rssi; });

	var html = "";
	var i;
	var row;
	var disp;
	var ch;
	var mhz;
	var dist;
	var clients;
	var vendor;
	var mac;

	for (i = 0; i < items.length; i++) {
		row = items[i];
		disp = apDisplayName(row.ap);
		ch = parseInt(row.ap[2], 10) || 0;
		mhz = channelToMhz(ch);
		dist = rssiToDistanceM(row.rssi);
		clients = countClientsForAp(row.idx);
		vendor = row.ap[6] || "";
		mac = row.ap[5] || "—";

		html += '<div class="ap-row' + (disp.hidden ? " hidden" : "")
			+ (row.idx === selectedApIdx ? " selected" : "")
			+ '" onclick="selectAp(' + row.idx + ')">'
			+ '<div class="ap-row-info">'
			+ '<div class="ap-row-ssid">' + esc(disp.text) + "</div>"
			+ '<div class="ap-row-line"><span class="lbl">MAC:</span> ' + esc(mac) + "</div>"
			+ '<div class="ap-row-line"><span class="lbl">Freq:</span> ' + mhz
			+ 'MHz <span class="ap-row-ch">CH ' + esc(String(ch)) + "</span></div>"
			+ '<div class="ap-row-enc"><span class="lock">&#128274;</span> '
			+ esc(formatEnc(row.ap[4])) + "</div>";
		if (vendor) {
			html += '<div class="ap-row-clients">' + esc(vendor);
			if (clients > 0) html += " · клиентов: " + clients;
			html += "</div>";
		} else if (clients > 0) {
			html += '<div class="ap-row-clients">клиентов: ' + clients + "</div>";
		}
		html += "</div>"
			+ '<div class="ap-row-side">'
			+ '<div class="ap-row-dist">' + dist + "m</div>"
			+ '<div class="ap-row-gauge">' + rssiGaugeSvg(row.rssi) + "</div>"
			+ "</div></div>";
	}
	el.innerHTML = html;
}

function updateStats() {
	var chSet = {};
	var i;
	for (i = 0; i < scanJson.aps.length; i++) {
		chSet[scanJson.aps[i][2]] = true;
	}
	var statAp = document.getElementById("statAp");
	var statSt = document.getElementById("statSt");
	var statCh = document.getElementById("statCh");
	if (statAp) statAp.textContent = scanJson.aps.length;
	if (statSt) statSt.textContent = scanJson.stations ? scanJson.stations.length : 0;
	if (statCh) statCh.textContent = Object.keys(chSet).length || "—";
}

function selectAp(idx) {
	selectedApIdx = idx;
	drawRadarBlips();
	drawApList();
}

function drawRadar() {
	drawRadarGrid();
	drawRadarSweep();
	drawRadarBlips();
	drawApList();
	updateStats();
}

var apScanBusy = false;

function reloadScanJson(silent) {
	getFile("run?cmd=save scan", function () {
		getFile("scan.json", function (res) {
			try {
				scanJson = JSON.parse(res);
				if (!scanJson.aps) scanJson.aps = [];
				if (!scanJson.stations) scanJson.stations = [];
			} catch (e) {
				scanJson = { aps: [], stations: [] };
			}
			drawRadar();
			if (!silent && typeof showActionToast === "function") {
				showActionToast("Радар: " + scanJson.aps.length + " AP", "ok");
			}
		}, 15000, "GET", function () {
			if (!silent && typeof showActionToast === "function") {
				showActionToast("Таймаут scan.json", "err");
			}
		});
	}, 8000, "GET", function () {
		if (!silent && typeof showActionToast === "function") {
			showActionToast("Ошибка save scan", "err");
		}
	});
}

function refreshRadar() {
	if (apScanBusy) return;
	apScanBusy = true;

	var btn = document.getElementById("btnRefresh");
	if (btn) btn.disabled = true;

	if (typeof showActionToast === "function") {
		showActionToast("Сканирование Wi-Fi сети", "info", 4000);
	}

	getFile("run?cmd=scan aps ", function () {
		setTimeout(function () {
			reloadScanJson(false);
			apScanBusy = false;
			if (btn) btn.disabled = false;
		}, 15000);
	}, 8000, "GET", function () {
		apScanBusy = false;
		if (btn) btn.disabled = false;
		if (typeof showActionToast === "function") {
			showActionToast("Не удалось запустить сканирование Wi-Fi сети", "err");
		}
	});
}
