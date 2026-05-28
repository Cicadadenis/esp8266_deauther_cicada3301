/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

var attackJSON = [[false, 0, 0], [false, 0, 0], [false, 0, 0]];

function draw() {
	getE("deauth").innerHTML = attackJSON[0][0] ? "<span class='desktop-only'>" + lang("stop") + "</span><span class='mobile-only'>Стоп</span>" : "<span class='desktop-only'>" + lang("start") + "</span><span class='mobile-only'>Старт</span>";
	getE("beacon").innerHTML = attackJSON[1][0] ? "<span class='desktop-only'>" + lang("stop") + "</span><span class='mobile-only'>Стоп</span>" : "<span class='desktop-only'>" + lang("start") + "</span><span class='mobile-only'>Старт</span>";
	getE("probe").innerHTML = attackJSON[2][0] ? "<span class='desktop-only'>" + lang("stop") + "</span><span class='mobile-only'>Стоп</span>" : "<span class='desktop-only'>" + lang("start") + "</span><span class='mobile-only'>Старт</span>";

	if (attackJSON[0][0]) getE("deauth").classList.add("active");
	else getE("deauth").classList.remove("active");
	if (attackJSON[1][0]) getE("beacon").classList.add("active");
	else getE("beacon").classList.remove("active");
	if (attackJSON[2][0]) getE("probe").classList.add("active");
	else getE("probe").classList.remove("active");

	getE("deauthTargets").innerHTML = esc(attackJSON[0][1] + "");
	getE("beaconTargets").innerHTML = esc(attackJSON[1][1] + "");
	getE("probeTargets").innerHTML = esc(attackJSON[2][1] + "");

	getE("deauthPkts").innerHTML = esc(attackJSON[0][2] + "/" + attackJSON[0][3]);
	getE("beaconPkts").innerHTML = esc(attackJSON[1][2] + "/" + attackJSON[1][3]);
	getE("probePkts").innerHTML = esc(attackJSON[2][2] + "/" + attackJSON[2][3]);

	getE("allpkts").innerHTML = esc(attackJSON[3] + "");
}

function stopAll() {
	showActionToast("Остановка всех атак…", "warn", 2000);
	getFile("run?cmd=stop attack", function () {
		showActionToast("Все атаки остановлены", "ok");
		load();
	});
}

function startDeauthAll() {
	uiConfirm({
		title: "Deauth All",
		message: "Массовый deauth ~60 с.\nТолько своя изолированная лабораторная среда!",
		confirmText: "Подтвердить",
		cancelText: "Отмена",
		danger: true,
		icon: "⚡"
	}).then(function (ok) {
		if (!ok) return;
		showActionToast("Deauth All ~60 с…", "warn", 2500);
		getFile("run?cmd=stop attack", function () {
			getFile("run?cmd=attack -da -t 60s", function () {
				attackJSON[0][0] = true;
				showActionToast("Deauth All запущен (~60 с)", "warn");
				setTimeout(load, 2000);
				draw();
			});
		});
	});
}

function start(mode) {
	switch (mode) {
		case 0:
			attackJSON[0][0] = !attackJSON[0][0];
			break;
		case 1:
			attackJSON[1][0] = !attackJSON[1][0];
			break;
		case 2:
			attackJSON[2][0] = !attackJSON[2][0];
			break;
	}
	var labels = ["Deauth", "Beacon", "Probe"];
	var active = attackJSON[mode][0];
	showActionToast(labels[mode] + (active ? " — старт" : " — стоп"), active ? "warn" : "ok", 2000);

	getFile("run?cmd=attack" + (attackJSON[0][0] ? " -d" : "") + (attackJSON[1][0] ? " -b" : "") + (attackJSON[2][0] ? " -p" : ""), function () {
		setTimeout(load, 2000);
		draw();
	});
}

function load() {
	getFile("attack.json", function (response) {
		attackJSON = JSON.parse(response);
		showMessage("connected");
		draw();
	});
}