/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

var ssidJson = { "random": false, "ssids": [] };

function load() {
	getFile("run?cmd=save ssids", function () {
		getFile("ssids.json", function (res) {
			ssidJson = JSON.parse(res);
			var maxSize = ssidJson.listSize || 100;
			getE("ssidListSize").value = maxSize;
			showMessage("connected");
			draw();
		});
	});
}

function setListSize() {
	var n = parseInt(getE("ssidListSize").value, 10);
	if (isNaN(n) || n < 1) n = 1;
	if (n > 255) n = 255;
	getE("ssidListSize").value = n;
	getFile("run?cmd=set ssid listsize " + n, function () {
		ssidJson.listSize = n;
		draw();
	});
}

function draw() {
	var html;

	html = "";

	for (var i = 0; i < ssidJson.ssids.length; i++) {
		var savedLength = ssidJson.ssids[i][2];
		var shownName = ssidJson.ssids[i][0];
		if (typeof savedLength === "number") shownName = shownName.substring(0, savedLength);

			html += "<div class='ssid-card'>"
			+ "<div class='ssid-name'>#" + i + " <input type='text' id='ssid_" + i + "' maxlength='32' value='" + esc(shownName) + "'></div>"
			+ "<div class='ssid-info'>"
			+ "<span>Security:</span>"
			+ "<span class='ssid-wpa' id='enc_" + i + "'>" + (ssidJson.ssids[i][1] ? "WPA2" : "OPEN") + "</span>"
			+ "</div>"
			+ "<div class='ssid-actions'>"
			+ "<button onclick='changeEnc(" + i + ")'><span class='desktop-only'>Вкл/выкл WPA2</span><span class='mobile-only'>WPA2</span></button>"
			+ "<button class='green' onclick='save(" + i + ")'><span class='desktop-only'>" + lang("save") + "</span><span class='mobile-only'>Сохранить</span></button>"
			+ "<button class='red' onclick='remove(" + i + ")'>Удалить</button>"
			+ "</div>"
			+ "</div>";
	}

	if (ssidJson.ssids.length === 0) {
		html = "<div class='ssid-card'><div class='ssid-name'>Нет сохранённых SSID</div></div>";
	}

	getE("randomBtn").innerHTML = ssidJson.random
		? "<span class='desktop-only'>" + lang("disable_random") + "</span><span class='mobile-only'>Рандом Выкл</span>"
		: "<span class='desktop-only'>" + lang("enable_random") + "</span><span class='mobile-only'>Рандом Вкл</span>";

	getE("ssidTable").innerHTML = html;
}

function remove(id) {
	ssidJson.ssids.splice(id, 1);
	getFile("run?cmd=remove ssid " + id);
	draw();
	showActionToast("SSID #" + id + " удалён", "ok");
}

function add() {
	var ssidStr = getE("ssid").value;
	var wpa2 = getE("enc").checked;
	var force = getE("overwrite").checked;
	var clones = 1;

	if (ssidStr.length > 0) {
		var cmdStr = "add ssid \"" + ssidStr + "\"" + (force ? " -f" : "") + " -cl " + clones;
		if (wpa2) cmdStr += " -wpa2";

		getFile("run?cmd=" + cmdStr);

		var maxSize = ssidJson.listSize || parseInt(getE("ssidListSize").value, 10) || 100;

		if (ssidJson.ssids.length >= maxSize && force) ssidJson.ssids.splice(0, 1);
		if (ssidJson.ssids.length < maxSize) ssidJson.ssids.push([ssidStr, wpa2, ssidStr.length]);

		draw();
		showActionToast("SSID добавлен", "ok");
	}
}

function enableRandom() {
	if (ssidJson.random) {
		showActionToast("Random SSID — выкл", "ok");
		getFile("run?cmd=disable random", function () {
			load();
		});
	} else {
		showActionToast("Random SSID — вкл", "warn", 2000);
		getFile("run?cmd=enable random " + getE("interval").value, function () {
			load();
		});
	}

}

function disableRandom() {

}

function countSelectedAps(scan) {
	var n = 0;
	var i;

	if (!scan || !scan.aps) return 0;

	for (i = 0; i < scan.aps.length; i++) {
		if (scan.aps[i][scan.aps[i].length - 1]) n++;
	}

	return n;
}

function addSelected() {
	getFile("run?cmd=save scan", function () {
		getFile("scan.json", function (res) {
			var scan;
			var sel;

			try {
				scan = JSON.parse(res);
			} catch (e) {
				scan = { aps: [] };
			}

			sel = countSelectedAps(scan);

			if (sel === 0) {
				showActionToast("Сначала выберите AP на вкладке «Скан»", "err");
				return;
			}

			showActionToast("Клонирование " + sel + " AP…", "info", 2200);
			getFile("run?cmd=add ssid -s" + (getE("overwrite").checked ? " -f" : ""), function () {
				showActionToast("SSID добавлены с выбранных AP", "ok");
				setTimeout(load, 800);
			}, 15000, "GET", function () {
				showActionToast("Таймаут клонирования SSID", "err");
			});
		}, 12000, "GET", function () {
			showActionToast("Нет scan.json — сначала сканируйте AP", "err");
		});
	});
}

function changeEnc(id) {
	ssidJson.ssids[id][1] = !ssidJson.ssids[id][1];
	draw();
	save(id);
}

function removeAll() {
	uiConfirm({
		title: "Удалить все SSID",
		message: "Удалить все SSID из списка?\nДействие нельзя отменить.",
		confirmText: "Подтвердить",
		cancelText: "Отмена",
		danger: true,
		icon: "🗑"
	}).then(function (ok) {
		if (!ok) return;
		ssidJson.ssids = [];
		getFile("run?cmd=remove ssids");
		draw();
		showActionToast("Список SSID очищен", "ok");
	});
}

function save(id) {
	var el = getE("ssid_" + id);
	var name = (el.value || el.innerHTML || "").replace("<br>", "").substring(0, 32);
	var wpa2 = ssidJson.ssids[id][1];
	ssidJson.ssids[id] = [name, wpa2, name.length];

	getFile("run?cmd=replace ssid " + id + " -n \"" + name + "\" " + (wpa2 ? "-wpa2" : ""));
	showActionToast("SSID #" + id + " сохранён", "ok");
}

