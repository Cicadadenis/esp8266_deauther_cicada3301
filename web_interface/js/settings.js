/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

var settingsJson = {};
var workmodeChangeBusy = false;
var portalPrefs = { captive: true, auth: true };
var portalPrefsSaved = { captive: true, auth: true };
var portalPrefsDirty = false;

function load() {
	getFile("settings.json", function (res) {
		settingsJson = JSON.parse(res);
		getFile("auth/password", function (pwd) {
			settingsJson.authPassword = pwd;
			getFile("portal/prefs.json", function (pp) {
				try { portalPrefs = JSON.parse(pp); } catch (e) { portalPrefs = { captive: true, auth: true }; }
				portalPrefsSaved = { captive: !!portalPrefs.captive, auth: !!portalPrefs.auth };
				portalPrefsDirty = false;
				showMessage("connected");
				draw();
			}, 8000, "GET", function () {
				portalPrefs = { captive: true, auth: true };
				portalPrefsSaved = { captive: true, auth: true };
				portalPrefsDirty = false;
				showMessage("connected");
				draw();
			});
		});
	});
}

function draw() {
	var html = "";

	// Extra switches that are not part of EEPROM settings.json
	html += "<div class='setting-card'>"
		+ "<div class='setting-title'>Portal</div>"
		+ "<p class='setting-desc'>Captive portal и авторизация. Применяются по кнопке «Сохранить». При включённом captive — разрыв Wi‑Fi для переподключения.</p>"
		+ "<label class='checkbox-label'>"
		+ "<input type='checkbox' " + (portalPrefs.captive ? "checked" : "") + " onchange='onPortalPrefChange(\"captive\",this.checked)'>"
		+ "<span>Captive portal (DNS/редиректы)</span>"
		+ "</label>"
		+ "<label class='checkbox-label'>"
		+ "<input type='checkbox' " + (portalPrefs.auth ? "checked" : "") + " onchange='onPortalPrefChange(\"auth\",this.checked)'>"
		+ "<span>Страница авторизации (/auth)</span>"
		+ "</label>"
		+ "</div>";

	for (var key in settingsJson) {
		if (settingsJson.hasOwnProperty(key)) {
			// Пропускаем display, displayTimeout и version
			if (key === "display" || key === "displayTimeout" || key === "version") {
				continue;
			}
			var safeKey = esc(key);
			html += "<div class='setting-card'>"
				+ "<div class='setting-title'>" + safeKey + "</div>"
				+ "<p class='setting-desc'>" + lang("setting_" + key) + "</p>";

			if (typeof settingsJson[key] == "boolean") {
				html += "<label class='checkbox-label'><input type='checkbox' name='" + safeKey + "' " + (settingsJson[key] ? "checked" : "") + " onchange='save(\"" + key + "\",this.checked)'><span>" + (settingsJson[key] ? "Enabled" : "Disabled") + "</span></label>";
			} else if (typeof settingsJson[key] == "number") {
				if (key === "workmode") {
					html += "<select name='" + safeKey + "' data-cyber-title='Режим устройства' onchange='onWorkmodeChange(this)'>"
						+ "<option value='0'" + (settingsJson[key] === 0 ? " selected" : "") + ">Deauther</option>"
						+ "<option value='1'" + (settingsJson[key] === 1 ? " selected" : "") + ">Ретранслятор</option>"
						+ "</select>";
				} else {
					html += "<input type='number' name='" + safeKey + "' value='" + esc(settingsJson[key]) + "' onchange='save(\"" + key + "\",parseInt(this.value,10))'>";
				}
			} else if (typeof settingsJson[key] == "string") {
				html += "<input type='text' name='" + safeKey + "' value='" + esc(settingsJson[key].toString()) + "' " + (key == "version" ? "readonly" : "") + " onchange='save(\"" + key + "\",this.value)'>";
			}

			html += "</div>";
		}
	}
	getE("settingsList").innerHTML = html;
	if (typeof initCyberSelects === "function") initCyberSelects(getE("settingsList"));
}

function onPortalPrefChange(key, value) {
	if (!key) return;
	portalPrefs[key] = !!value;
	portalPrefsDirty = true;
	showActionToast("Portal: нажмите «Сохранить»", "info", 2200);
}

function flushPortalPrefs(onOk, onErr, withBounce) {
	var qs = "portal/prefs?captive=" + (portalPrefs.captive ? "1" : "0")
		+ "&auth=" + (portalPrefs.auth ? "1" : "0");
	if (withBounce && portalPrefs.captive) {
		qs += "&bounce=1";
	}
	getFile(qs, onOk, 12000, "GET", onErr);
}

function onWorkmodeChange(sel) {
	if (!sel || workmodeChangeBusy) return;

	var newMode = parseInt(sel.value, 10);
	var prevMode = (typeof settingsJson.workmode === "number") ? settingsJson.workmode : 0;
	if (isNaN(newMode)) newMode = 0;
	if (newMode === prevMode) return;

	var toRepeater = newMode === 1;
	var title = toRepeater ? "Режим: Ретранслятор" : "Режим: Deauther";
	var message = toRepeater
		? "Переключить устройство в режим «Ретранслятор»?\n\n"
			+ "Скрываются вкладки Deauther, появляется «Ретранслятор».\n"
			+ "Настройки будут сохранены, затем ESP перезагрузится."
		: "Вернуть режим «Deauther»?\n\n"
			+ "Ретранслятор будет отключён.\n"
			+ "Настройки будут сохранены, затем ESP перезагрузится.";

	uiConfirm({
		title: title,
		message: message,
		confirmText: "Сменить и перезагрузить",
		cancelText: "Отмена",
		danger: toRepeater,
		icon: toRepeater ? "📡" : "⚡"
	}).then(function (ok) {
		if (!ok) {
			sel.value = String(prevMode);
			return;
		}

		workmodeChangeBusy = true;
		settingsJson.workmode = newMode;
		showActionToast("Сохранение режима…", "info", 2500);

		getFile("run?cmd=set workmode \"" + newMode + "\"", function () {
			getFile("run?cmd=save settings", function () {
				if (typeof markReconnectAfterReboot === "function") {
					markReconnectAfterReboot(!!portalPrefs.captive);
				}
				if (portalPrefs.captive) {
					showActionToast("Перезагрузка… Подключитесь к Wi‑Fi снова", "warn", 8000);
				} else {
					showActionToast("Перезагрузка… Страница обновится после переподключения", "warn", 8000);
				}
				getFile("run?cmd=reboot");
			}, 12000, "GET", function () {
				workmodeChangeBusy = false;
				sel.value = String(prevMode);
				settingsJson.workmode = prevMode;
				showActionToast("Не удалось сохранить настройки", "err");
			});
		}, 8000, "GET", function () {
			workmodeChangeBusy = false;
			sel.value = String(prevMode);
			settingsJson.workmode = prevMode;
			showActionToast("Не удалось применить режим", "err");
		});
	});
}

function save(key, value) {
	if (key) {
		settingsJson[key] = value;

		if (key == "authPassword") {
			getFile("auth/password?value=" + encodeURIComponent(value));
			showActionToast("Пароль веб-интерфейса обновлён", "ok");
			return;
		}

		getFile("run?cmd=set " + key + " \"" + value + "\"");
		showActionToast("Параметр «" + key + "» применён (нажмите Сохранить)", "info", 1800);
	} else {
		showActionToast("Сохранение…", "info", 1500);
		getFile("run?cmd=save settings", function (res) {
			var captiveWillBounce = !!portalPrefs.captive;

			var finishSave = function () {
				portalPrefsDirty = false;
				portalPrefsSaved = { captive: !!portalPrefs.captive, auth: !!portalPrefs.auth };
				if (captiveWillBounce) {
					showActionToast("Сохранено. Подключитесь к Wi‑Fi снова", "ok", 6000);
				} else {
					showActionToast("Настройки сохранены", "ok");
				}
				load();
			};
			var onPortalFail = function () {
				showActionToast("EEPROM OK, но portal не сохранился", "err", 5000);
				load();
			};

			if (captiveWillBounce && typeof markReconnectAfterReboot === "function") {
				markReconnectAfterReboot(true);
			}
			// Всегда синхронизируем portal с устройством; bounce=1 при включённом captive.
			flushPortalPrefs(finishSave, onPortalFail, true);
		});
	}
}

function resetSettings() {
	uiConfirm({
		title: "Сброс настроек",
		message: "Сбросить все настройки к заводским?\nТекущие значения будут перезаписаны и сохранены в EEPROM.",
		confirmText: "Подтвердить",
		cancelText: "Отмена",
		danger: true,
		icon: "↺"
	}).then(function (ok) {
		if (!ok) return;
		showActionToast("Сброс настроек…", "warn", 2500);
		getFile("run?cmd=reset;;save settings", function () {
			showActionToast("Заводские настройки применены", "ok");
			setTimeout(load, 800);
		});
	});
}

function rebootDevice() {
	uiConfirm({
		title: "Перезагрузка",
		message: "Перезагрузить ESP8266?\nWi‑Fi отключится на несколько секунд.",
		confirmText: "Подтвердить",
		cancelText: "Отмена",
		danger: true,
		icon: "⟳"
	}).then(function (ok) {
		if (!ok) return;
		if (typeof markReconnectAfterReboot === "function") {
			markReconnectAfterReboot(!!portalPrefs.captive);
		}
		if (portalPrefs.captive) {
			showActionToast("Перезагрузка… Подключитесь к Wi‑Fi снова", "warn", 5000);
		} else {
			showActionToast("Перезагрузка… Страница обновится после переподключения", "warn", 5000);
		}
		getFile("run?cmd=reboot");
	});
}

function stopWifiAp() {
	showActionToast("Wi‑Fi AP выключается…", "warn", 2800);
	getFile("run?cmd=stopap");
}

function uploadFirmware() {
	var fileInput = getE("fwFile");
	var progressEl = getE("fwProgress");
	var uploadBtn = getE("fwUploadBtn");
	var file;

	if (!fileInput || !fileInput.files || fileInput.files.length === 0) {
		showActionToast("Выберите файл .bin", "err");
		return;
	}

	file = fileInput.files[0];

	if (!/\.bin$/i.test(file.name)) {
		showActionToast("Нужен файл с расширением .bin", "err");
		return;
	}

	uiConfirm({
		title: "Обновление прошивки",
		message: "Прошить ESP файлом\n«" + file.name + "» (" + Math.round(file.size / 1024) + " КБ)?\n\n"
			+ "Устройство перезагрузится. Не выключайте питание до завершения.",
		confirmText: "Прошить",
		cancelText: "Отмена",
		danger: true,
		icon: "⬆"
	}).then(function (ok) {
		if (!ok) return;

		var xhr = new XMLHttpRequest();
		var fd = new FormData();

		if (uploadBtn) uploadBtn.disabled = true;
		if (progressEl) progressEl.textContent = "Подготовка…";

		xhr.open("POST", "/update", true);
		xhr.timeout = 300000;

		xhr.upload.onprogress = function (e) {
			if (!progressEl || !e.lengthComputable) return;
			progressEl.textContent = "Загрузка: " + Math.round((e.loaded / e.total) * 100) + "%";
		};

		xhr.onreadystatechange = function () {
			if (xhr.readyState !== 4) return;

			if (uploadBtn) uploadBtn.disabled = false;

			if (xhr.status === 200 && xhr.responseText.indexOf("OK") >= 0) {
				if (progressEl) progressEl.textContent = "Готово — перезагрузка…";
				showActionToast("Прошивка установлена", "ok", 5000);
				return;
			}

			if (xhr.status === 400 && (xhr.responseText || "").indexOf("VALIDATION_FAIL") >= 0) {
				if (progressEl) progressEl.textContent = "";
				showActionToast("Этот файл не является обновлением прошивки (валидация не пройдена)", "err", 7000);
				return;
			}

			if (progressEl) progressEl.textContent = "";
			showActionToast(xhr.responseText || "Ошибка прошивки (код " + xhr.status + ")", "err", 6000);
		};

		xhr.ontimeout = function () {
			if (uploadBtn) uploadBtn.disabled = false;
			if (progressEl) progressEl.textContent = "";
			showActionToast("Таймаут загрузки прошивки", "err");
		};

		xhr.onerror = function () {
			if (uploadBtn) uploadBtn.disabled = false;
			if (progressEl) progressEl.textContent = "";
			showActionToast("Ошибка сети при загрузке", "err");
		};

		fd.append("update", file, file.name);
		if (progressEl) progressEl.textContent = "Отправка…";
		xhr.send(fd);
	});
}
