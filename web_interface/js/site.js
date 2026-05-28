/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

var langJson = {};

function getE(name) {
	return document.getElementById(name);
}

function esc(str) {
	if (str) {
		return str.toString()
			.replace(/&/g, '&amp;')
			.replace(/</g, '&lt;')
			.replace(/>/g, '&gt;')
			.replace(/\"/g, '&quot;')
			.replace(/\'/g, '&#39;')
			.replace(/\//g, '&#x2F;');
	}
	return "";
}

function convertLineBreaks(str) {
	if (str) {
		str = str.toString();
		str = str.replace(/(?:\r\n|\r|\n)/g, '<br>');
		return str;
	}
	return "";
}

function showMessage(msg) {
	var statusEl = getE("status");

	if (msg.startsWith("ERROR")) {
		if (statusEl) {
			statusEl.style.backgroundColor = "#d33";
			statusEl.innerHTML = "disconnected";
		}

		console.error("disconnected (" + msg + ")");
	} else if (msg.startsWith("LOADING")) {
		if (statusEl) {
			statusEl.style.backgroundColor = "#fc0";
			statusEl.innerHTML = "loading...";
		}
	} else {
		if (statusEl) {
			statusEl.style.backgroundColor = "#3c5";
			statusEl.innerHTML = "connected";
		}

		console.log("" + msg + "");
	}
}

function getFile(adr, callback, timeout, method, onTimeout, onError) {
	/* fallback stuff */
	if (adr === undefined) return;
	if (callback === undefined) callback = function () { };
	if (timeout === undefined) timeout = 8000;
	if (method === undefined) method = "GET";
	if (onTimeout === undefined) {
		onTimeout = function () {
			showMessage("ERROR: timeout loading file " + adr);
		};
	}
	if (onError === undefined) {
		onError = function () {
			showMessage("ERROR: loading file: " + adr);
		};
	}

	/* create request */
	var request = new XMLHttpRequest();

	/* set parameter for request */
	request.open(method, encodeURI(adr), true);
	request.timeout = timeout;
	request.ontimeout = onTimeout;
	request.onerror = onError;
	request.overrideMimeType("application/json");

	request.onreadystatechange = function () {
		if (this.readyState != 4) return;
		if (this.status === 0) {
			if (onError) onError();
			return;
		}
		showMessage(this.status === 200 ? "CONNECTED" : ("HTTP " + this.status));
		callback(this.responseText);
	};

	showMessage("LOADING");

	/* send request */
	request.send();

	console.log(adr);
}

function lang(key) {
	return convertLineBreaks(esc(langJson[key]));
}

function parseLang(fileStr) {
	langJson = JSON.parse(fileStr);
	if (langJson["lang"] != "en") {// no need to update the HTML	
		var elements = document.querySelectorAll("[data-translate]");
		for (i = 0; i < elements.length; i++) {
			var element = elements[i];
			element.innerHTML = lang(element.getAttribute("data-translate"));
		}
	}
	document.querySelector('html').setAttribute("lang", langJson["lang"]);
	applyWorkmodeNav();
	if (typeof load !== 'undefined') load();
}

var DEVICE_WAIT_KEY = "repWaitReconnect";
var deviceLinkWasLost = false;
var deviceLinkWatchTimer = null;

function markReconnectAfterReboot(captiveEnabled) {
	try {
		if (captiveEnabled) {
			sessionStorage.removeItem(DEVICE_WAIT_KEY);
		} else {
			sessionStorage.setItem(DEVICE_WAIT_KEY, "1");
		}
	} catch (e) { }
}

function pingDevice(ok, fail) {
	var xhr = new XMLHttpRequest();
	xhr.open("GET", "/settings.json?_=" + Date.now(), true);
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

function pollDeviceLink() {
	pingDevice(function () {
		var waiting = false;
		try { waiting = sessionStorage.getItem(DEVICE_WAIT_KEY) === "1"; } catch (e) { }

		if (deviceLinkWasLost || waiting) {
			try { sessionStorage.removeItem(DEVICE_WAIT_KEY); } catch (e) { }
			location.reload();
			return;
		}
	}, function () {
		deviceLinkWasLost = true;
	});
}

function beginDeviceReconnectWatch(forceLost) {
	if (forceLost) deviceLinkWasLost = true;
	if (deviceLinkWatchTimer) return;
	deviceLinkWatchTimer = setInterval(pollDeviceLink, 2500);
	if (!window.__deviceOnlineHook) {
		window.__deviceOnlineHook = true;
		window.addEventListener("online", pollDeviceLink);
	}
}

function applyWorkmodeNav() {
	getFile("settings.json", function (res) {
		var s = {};
		try { s = JSON.parse(res); } catch (e) { s = {}; }

		var mode = (typeof s.workmode === "number") ? s.workmode : 0;
		if (mode !== 1) return;

		var path = (window.location.pathname || "").toLowerCase();
		var file = path.split("/").pop();
		if (file.slice(-5) === ".html") file = file.slice(0, -5);
		var wait = "";
		try {
			if (sessionStorage.getItem(DEVICE_WAIT_KEY) === "1") wait = "?wait=1";
		} catch (e) { }

		if (file === "index" || file === "index.htm" || file === "") {
			window.location.href = "repeater" + wait;
			return;
		}

		var allow = { "settings": true, "info": true, "repeater": true };
		if (!allow[file]) {
			window.location.href = "repeater" + wait;
			return;
		}

		var menu = document.querySelector("nav .menu");
		if (!menu) return;

		var hideHrefs = { "scan": true, "radar": true, "ssids": true, "attack": true };
		var links = menu.querySelectorAll("a");
		for (var i = 0; i < links.length; i++) {
			var a = links[i];
			var href = (a.getAttribute("href") || "").toLowerCase();
			if (hideHrefs[href]) {
				if (a.parentElement) a.parentElement.style.display = "none";
			}
		}

		var existing = menu.querySelector("a[href='repeater'], a[href='repeater#']");
		if (!existing) {
			var li = document.createElement("li");
			var a = document.createElement("a");
			a.href = "repeater";
			a.textContent = "Ретранслятор";
			li.appendChild(a);
			menu.insertBefore(li, menu.firstChild);
		}
	}, 2500);
}

function loadLang() {
	var language = "ru";
	getFile("lang/" + language + ".lang",
		parseLang,
		2000,
		"GET",
		function () {
			getFile("lang/en.lang", parseLang);
		}, function () {
			getFile("lang/en.lang", parseLang);
		}
	);
}

var actionToastTimer = null;

function showActionToast(message, type, durationMs) {
	var ms = durationMs || 2200;
	var el = document.getElementById("actionToast");

	if (!el) {
		el = document.createElement("div");
		el.id = "actionToast";
		el.setAttribute("role", "status");
		el.setAttribute("aria-live", "polite");
		document.body.appendChild(el);

		if (!document.getElementById("actionToastStyle")) {
			var st = document.createElement("style");
			st.id = "actionToastStyle";
			st.textContent = "#actionToast{position:fixed;left:50%;bottom:max(5.5rem, calc(72px + env(safe-area-inset-bottom)));transform:translateX(-50%) translateY(16px);z-index:10050;max-width:min(92vw,360px);padding:.85rem 1.2rem;border-radius:12px;font-family:'JetBrains Mono',monospace;font-size:.8rem;line-height:1.4;text-align:center;opacity:0;pointer-events:none;transition:opacity .3s ease,transform .3s ease;box-shadow:0 8px 32px rgba(0,0,0,.45)}"
				+ "#actionToast.open{opacity:1;transform:translateX(-50%) translateY(0)}"
				+ "#actionToast.ok{background:rgba(0,255,157,.92);color:#0a0a12;border:1px solid rgba(0,255,157,.6)}"
				+ "#actionToast.warn{background:rgba(255,170,0,.92);color:#1a1000;border:1px solid rgba(255,170,0,.5)}"
				+ "#actionToast.err{background:rgba(255,51,102,.92);color:#fff;border:1px solid rgba(255,51,102,.5)}"
				+ "#actionToast.info{background:rgba(20,20,45,.95);color:#e0e0ff;border:1px solid rgba(0,212,255,.35)}"
				+ "@media (min-width:769px){#actionToast{bottom:1.5rem}}";
			document.head.appendChild(st);
		}
	}

	el.textContent = message;
	el.className = "open " + (type || "info");

	clearTimeout(actionToastTimer);
	actionToastTimer = setTimeout(function () {
		el.classList.remove("open");
	}, ms);
}

/* ——— Кастомные confirm / alert (вместо системных окон браузера) ——— */
function injectUiDialogStyles() {
	if (document.getElementById("uiDialogStyle")) return;
	var st = document.createElement("style");
	st.id = "uiDialogStyle";
	st.textContent = ""
		+ ".ui-dialog-root{position:fixed;inset:0;z-index:20000;display:flex;align-items:center;justify-content:center;padding:max(1rem,env(safe-area-inset-top)) max(1rem,env(safe-area-inset-right)) max(1rem,env(safe-area-inset-bottom)) max(1rem,env(safe-area-inset-left));opacity:0;visibility:hidden;pointer-events:none;transition:opacity .28s ease,visibility .28s ease}"
		+ ".ui-dialog-root.open{opacity:1;visibility:visible;pointer-events:auto}"
		+ ".ui-dialog-backdrop{position:absolute;inset:0;background:rgba(0,0,0,.72);backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px)}"
		+ ".ui-dialog-panel{position:relative;z-index:1;width:min(92vw,400px);background:linear-gradient(145deg,rgba(18,8,40,.97),rgba(30,15,55,.98));border:1px solid rgba(120,80,255,.45);border-radius:18px;padding:1.35rem 1.25rem 1.1rem;box-shadow:0 0 40px rgba(160,32,240,.25),0 20px 50px rgba(0,0,0,.55);transform:scale(.92) translateY(12px);transition:transform .3s cubic-bezier(.16,1,.3,1)}"
		+ ".ui-dialog-root.open .ui-dialog-panel{transform:scale(1) translateY(0)}"
		+ ".ui-dialog-icon{width:44px;height:44px;margin:0 auto .85rem;border-radius:50%;display:flex;align-items:center;justify-content:center;font-size:1.35rem;border:1px solid rgba(0,240,255,.35);background:rgba(0,240,255,.08);color:#00f0ff;box-shadow:0 0 20px rgba(0,240,255,.2)}"
		+ ".ui-dialog-root.danger .ui-dialog-icon{border-color:rgba(255,51,102,.45);background:rgba(255,51,102,.1);color:#ff6688;box-shadow:0 0 20px rgba(255,51,102,.25)}"
		+ ".ui-dialog-title{font-family:'Orbitron',sans-serif;font-size:1rem;font-weight:700;letter-spacing:.06em;text-align:center;color:#00f0ff;text-shadow:0 0 12px rgba(0,240,255,.35);margin-bottom:.65rem;line-height:1.3}"
		+ ".ui-dialog-root.danger .ui-dialog-title{color:#ff88aa;text-shadow:0 0 12px rgba(255,51,102,.35)}"
		+ ".ui-dialog-message{font-family:'JetBrains Mono',monospace;font-size:.82rem;line-height:1.55;color:#d8c8f0;text-align:center;margin-bottom:1.15rem;white-space:pre-wrap;word-break:break-word}"
		+ ".ui-dialog-actions{display:flex;gap:.65rem;justify-content:center;flex-wrap:wrap}"
		+ ".ui-dialog-btn{flex:1 1 120px;min-width:110px;padding:.72rem 1rem;border-radius:10px;border:1px solid rgba(120,80,255,.4);font-family:'Orbitron',sans-serif;font-size:.72rem;font-weight:700;letter-spacing:.08em;text-transform:uppercase;cursor:pointer;transition:transform .2s ease,box-shadow .2s ease,background .2s ease}"
		+ ".ui-dialog-btn:active{transform:scale(.97)}"
		+ ".ui-dialog-btn-cancel{background:linear-gradient(135deg,#ff3366,#c02040);color:#fff;border-color:rgba(255,51,102,.55)}"
		+ ".ui-dialog-btn-cancel:hover{box-shadow:0 0 22px rgba(255,51,102,.45)}"
		+ ".ui-dialog-btn-ok{background:linear-gradient(135deg,#00ff9d,#00c878);color:#0a1210;border-color:rgba(0,255,157,.55)}"
		+ ".ui-dialog-btn-ok:hover{box-shadow:0 0 22px rgba(0,255,157,.45)}"
		+ ".ui-dialog-root.alert-only .ui-dialog-btn-cancel{display:none}"
		+ ".ui-dialog-root.alert-only .ui-dialog-btn-ok{flex:1 1 100%}";
	document.head.appendChild(st);
}

function ensureUiDialogRoot() {
	injectUiDialogStyles();
	var root = document.getElementById("uiDialogRoot");
	if (root) return root;

	root = document.createElement("div");
	root.id = "uiDialogRoot";
	root.className = "ui-dialog-root";
	root.setAttribute("aria-hidden", "true");
	root.innerHTML = ""
		+ "<div class=\"ui-dialog-backdrop\" data-ui-close></div>"
		+ "<div class=\"ui-dialog-panel\" role=\"dialog\" aria-modal=\"true\" aria-labelledby=\"uiDialogTitle\">"
		+ "<div class=\"ui-dialog-icon\" id=\"uiDialogIcon\">?</div>"
		+ "<div class=\"ui-dialog-title\" id=\"uiDialogTitle\"></div>"
		+ "<div class=\"ui-dialog-message\" id=\"uiDialogMessage\"></div>"
		+ "<div class=\"ui-dialog-actions\">"
		+ "<button type=\"button\" class=\"ui-dialog-btn ui-dialog-btn-cancel\" id=\"uiDialogCancel\">Отмена</button>"
		+ "<button type=\"button\" class=\"ui-dialog-btn ui-dialog-btn-ok\" id=\"uiDialogOk\">OK</button>"
		+ "</div></div>";
	document.body.appendChild(root);

	var finish = function (value) {
		root.classList.remove("open");
		root.setAttribute("aria-hidden", "true");
		document.body.style.overflow = "";
		if (uiDialogResolve) {
			var r = uiDialogResolve;
			uiDialogResolve = null;
			r(value);
		}
	};

	root.querySelector("[data-ui-close]").addEventListener("click", function () { finish(false); });
	getE("uiDialogCancel").addEventListener("click", function () { finish(false); });
	getE("uiDialogOk").addEventListener("click", function () { finish(true); });

	root.addEventListener("keydown", function (e) {
		if (!root.classList.contains("open")) return;
		if (e.key === "Escape") finish(false);
		if (e.key === "Enter") finish(true);
	});

	return root;
}

var uiDialogResolve = null;

function uiConfirm(options) {
	options = options || {};
	var title = options.title || "Подтверждение";
	var message = options.message || "";
	var confirmText = options.confirmText || "Подтвердить";
	var cancelText = options.cancelText || "Отмена";
	var danger = !!options.danger;
	var icon = options.icon || (danger ? "⚠" : "◆");

	return new Promise(function (resolve) {
		var root = ensureUiDialogRoot();
		uiDialogResolve = resolve;

		root.className = "ui-dialog-root open" + (danger ? " danger" : "");
		root.setAttribute("aria-hidden", "false");
		document.body.style.overflow = "hidden";

		getE("uiDialogIcon").textContent = icon;
		getE("uiDialogTitle").textContent = title;
		getE("uiDialogMessage").textContent = message;
		getE("uiDialogOk").textContent = confirmText;
		getE("uiDialogCancel").textContent = cancelText;
		getE("uiDialogCancel").style.display = "";

		setTimeout(function () { getE("uiDialogOk").focus(); }, 50);
	});
}

function uiAlert(options) {
	options = options || {};
	return new Promise(function (resolve) {
		var root = ensureUiDialogRoot();
		uiDialogResolve = function () { resolve(true); };

		root.className = "ui-dialog-root open alert-only" + (options.danger ? " danger" : "");
		root.setAttribute("aria-hidden", "false");
		document.body.style.overflow = "hidden";

		getE("uiDialogIcon").textContent = options.icon || "ℹ";
		getE("uiDialogTitle").textContent = options.title || "Сообщение";
		getE("uiDialogMessage").textContent = options.message || "";
		getE("uiDialogOk").textContent = options.confirmText || "Подтвердить";
		getE("uiDialogCancel").style.display = "none";

		setTimeout(function () { getE("uiDialogOk").focus(); }, 50);
	});
}

/** Совместимость: uiConfirm с одной строкой как message */
window.uiConfirm = uiConfirm;
window.uiAlert = uiAlert;

/* ——— Кастомный select (вместо системного окна на Android/iOS) ——— */
var cyberPickerActiveSelect = null;

function ensureCyberPickerRoot() {
	var root = document.getElementById("cyberPickerRoot");
	if (root) return root;

	root = document.createElement("div");
	root.id = "cyberPickerRoot";
	root.className = "cyber-picker-root";
	root.setAttribute("aria-hidden", "true");
	// Some pages (e.g. splash/index) don't include cicada_theme.css.
	// Ensure the picker root stays invisible unless explicitly opened.
	root.style.display = "none";
	root.innerHTML = ""
		+ "<div class=\"cyber-picker-backdrop\" data-cyber-picker-close></div>"
		+ "<div class=\"cyber-picker-sheet\" role=\"dialog\" aria-modal=\"true\">"
		+ "<div class=\"cyber-picker-head\"><div class=\"cyber-picker-title\" id=\"cyberPickerTitle\">Выбор</div></div>"
		+ "<div class=\"cyber-picker-list\" id=\"cyberPickerList\"></div>"
		+ "<button type=\"button\" class=\"cyber-picker-close\" data-cyber-picker-close>Закрыть</button>"
		+ "</div>";
	document.body.appendChild(root);

	root.addEventListener("click", function (e) {
		if (e.target && e.target.getAttribute("data-cyber-picker-close") !== null) {
			closeCyberPicker();
		}
	});

	return root;
}

function closeCyberPicker() {
	var root = document.getElementById("cyberPickerRoot");
	if (!root) return;
	root.classList.remove("open");
	root.setAttribute("aria-hidden", "true");
	root.style.display = "none";
	document.body.style.overflow = "";
	cyberPickerActiveSelect = null;
}

function syncCyberSelectTrigger(selectEl) {
	if (!selectEl || !selectEl._cyberTrigger) return;
	var opt = selectEl.options[selectEl.selectedIndex];
	selectEl._cyberTrigger.querySelector(".cyber-select-value").textContent = opt ? opt.textContent : "—";
}

function openCyberPicker(selectEl) {
	if (!selectEl) return;

	var root = ensureCyberPickerRoot();
	root.style.display = "flex";
	var titleEl = document.getElementById("cyberPickerTitle");
	var listEl = document.getElementById("cyberPickerList");
	var label = selectEl.getAttribute("data-cyber-title");
	var i;
	var opt;
	var btn;

	if (!label && selectEl.id) {
		var lbl = document.querySelector('label[for="' + selectEl.id + '"]');
		if (lbl) label = lbl.textContent;
	}
	titleEl.textContent = (label || "Выберите значение").trim();

	listEl.innerHTML = "";
	for (i = 0; i < selectEl.options.length; i++) {
		opt = selectEl.options[i];
		btn = document.createElement("button");
		btn.type = "button";
		btn.className = "cyber-picker-option" + (opt.selected ? " selected" : "");
		btn.innerHTML = "<span class=\"cyber-picker-label\">" + esc(opt.textContent) + "</span><span class=\"cyber-picker-mark\" aria-hidden=\"true\"></span>";
		btn.setAttribute("data-value", opt.value);
		btn.addEventListener("click", function () {
			selectEl.value = this.getAttribute("data-value");
			syncCyberSelectTrigger(selectEl);
			if (typeof Event === "function") {
				selectEl.dispatchEvent(new Event("change", { bubbles: true }));
			} else if (selectEl.onchange) {
				selectEl.onchange();
			}
			closeCyberPicker();
		});
		listEl.appendChild(btn);
	}

	cyberPickerActiveSelect = selectEl;
	root.classList.add("open");
	root.setAttribute("aria-hidden", "false");
	document.body.style.overflow = "hidden";
}

function enhanceCyberSelect(selectEl) {
	if (!selectEl || selectEl.getAttribute("data-cyber-done") === "1") return;
	if (selectEl.getAttribute("data-cyber-skip") === "1") return;

	selectEl.setAttribute("data-cyber-done", "1");
	selectEl.classList.add("cyber-select-native");

	var wrap = document.createElement("div");
	wrap.className = "cyber-select-wrap";
	selectEl.parentNode.insertBefore(wrap, selectEl);
	wrap.appendChild(selectEl);

	var trigger = document.createElement("button");
	trigger.type = "button";
	trigger.className = "cyber-select-trigger";
	trigger.innerHTML = "<span class=\"cyber-select-value\"></span><span class=\"cyber-select-chevron\" aria-hidden=\"true\"></span>";
	trigger.addEventListener("click", function (e) {
		e.preventDefault();
		openCyberPicker(selectEl);
	});

	wrap.insertBefore(trigger, selectEl);
	selectEl._cyberTrigger = trigger;
	selectEl.addEventListener("change", function () {
		syncCyberSelectTrigger(selectEl);
	});
	syncCyberSelectTrigger(selectEl);
}

function initCyberSelects(root) {
	root = root || document;
	var nodes = root.querySelectorAll ? root.querySelectorAll("select:not([data-cyber-done])") : [];
	var i;
	for (i = 0; i < nodes.length; i++) {
		enhanceCyberSelect(nodes[i]);
	}
}

function initCyberFileInputs(root) {
	root = root || document;
	var inputs = root.querySelectorAll ? root.querySelectorAll("input[type=file].cyber-file-input") : [];
	var i;
	var inp;
	var nameEl;

	for (i = 0; i < inputs.length; i++) {
		inp = inputs[i];
		if (inp.getAttribute("data-cyber-file-done") === "1") continue;
		inp.setAttribute("data-cyber-file-done", "1");
		nameEl = document.getElementById(inp.getAttribute("data-cyber-file-label"));
		if (!nameEl) continue;

		inp.addEventListener("change", function () {
			var el = document.getElementById(this.getAttribute("data-cyber-file-label"));
			if (!el) return;
			if (this.files && this.files.length > 0) {
				el.textContent = this.files[0].name;
				el.classList.add("has-file");
			} else {
				el.textContent = "Файл не выбран";
				el.classList.remove("has-file");
			}
		});
	}
}

window.initCyberSelects = initCyberSelects;
window.initCyberFileInputs = initCyberFileInputs;

// Some pages (e.g. info) don't call loadLang().
// Still apply repeater workmode navigation rules.
document.addEventListener("DOMContentLoaded", function () {
	var waitReconnect = false;
	try { waitReconnect = sessionStorage.getItem(DEVICE_WAIT_KEY) === "1"; } catch (e) { }
	if (/[?&]wait=1(?:&|$)/.test(location.search || "")) waitReconnect = true;
	if (waitReconnect) beginDeviceReconnectWatch(true);

	try { applyWorkmodeNav(); } catch (e) { }
	if (waitReconnect) pollDeviceLink();
});

window.addEventListener('load', function () {
	ensureUiDialogRoot();
	ensureCyberPickerRoot();
	initCyberSelects(document);
	initCyberFileInputs(document);
	var statusEl = getE("status");
	if (statusEl) {
		statusEl.style.backgroundColor = "#3c5";
		statusEl.innerHTML = "connected";
	}
});
