(function () {
    "use strict";

    var MAX_LOG = 100;
    var MAX_RFID = 20;
    var RECONNECT_MS = 3000;

    var ws = null;
    var reconnectTimer = null;

    /* Element references */
    var el = {};
    var ids = [
        "conn-status", "conn-text", "mqtt-status",
        "dev-imei", "dev-status", "dev-lastseen", "dev-topic-type",
        "dev-ssid", "dev-channel", "dev-mac", "dev-clients-num",
        "dev-battery", "dev-gsm", "dev-gps", "dev-speed",
        "dev-sats", "dev-acc", "dev-movement", "dev-alarm",
        "client-count", "clients-body", "rfid-body", "log-entries",
        "topic-input", "btn-subscribe", "active-subs"
    ];
    ids.forEach(function (id) {
        el[id] = document.getElementById(id);
    });

    function setConnected(connected) {
        el["conn-status"].className = "status-indicator " + (connected ? "connected" : "disconnected");
        el["conn-text"].textContent = connected ? "WS" : "WS";
    }

    function setMqttActive() {
        el["mqtt-status"].className = "status-indicator connected";
    }

    function now() {
        return new Date().toLocaleTimeString("en-GB", { hour12: false });
    }

    function escapeHtml(s) {
        if (typeof s !== "string") s = String(s);
        var d = document.createElement("div");
        d.appendChild(document.createTextNode(s));
        return d.innerHTML;
    }

    /* ── WebSocket commands (browser → server) ── */
    function sendCommand(obj) {
        if (ws && ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify(obj));
        }
    }

    function subscribeTopic(topic) {
        if (!topic || !topic.trim()) return;
        sendCommand({ action: "subscribe", topic: topic.trim() });
    }

    function unsubscribeTopic(topic) {
        sendCommand({ action: "unsubscribe", topic: topic });
    }

    function requestSubList() {
        sendCommand({ action: "list_subscriptions" });
    }

    /* ── Render active subscriptions as tags ── */
    function renderActiveSubs(subs) {
        var container = el["active-subs"];
        container.innerHTML = "";
        if (!subs || subs.length === 0) {
            container.innerHTML = '<span class="sub-empty">No active subscriptions</span>';
            return;
        }
        subs.forEach(function (topic) {
            var tag = document.createElement("span");
            tag.className = "sub-tag";
            tag.innerHTML = escapeHtml(topic) +
                ' <button class="sub-remove" title="Unsubscribe">&times;</button>';
            tag.querySelector(".sub-remove").addEventListener("click", function () {
                unsubscribeTopic(topic);
            });
            container.appendChild(tag);
        });
    }

    /* ── Detect topic type from topic string ── */
    function getTopicType(topic) {
        if (!topic) return "unknown";
        if (topic.indexOf("/heartbeat/") !== -1) return "heartbeat";
        if (topic.indexOf("/wifi/") !== -1) return "wifi";
        if (topic.indexOf("/rfid/") !== -1) return "rfid";
        if (topic.indexOf("/login/") !== -1) return "login";
        return "unknown";
    }

    /* ── Extract IMEI from topic path ── */
    function getImeiFromTopic(topic) {
        if (!topic) return null;
        var parts = topic.split("/");
        var last = parts[parts.length - 1];
        if (/^\d{10,}$/.test(last)) return last;
        return null;
    }

    /* ── Update common device info (any message) ── */
    function updateCommon(topic, payload, receivedAt) {
        var type = getTopicType(topic);
        el["dev-topic-type"].textContent = type;
        el["dev-status"].textContent = "Online";
        el["dev-lastseen"].textContent = receivedAt
            ? new Date(receivedAt).toLocaleString("en-GB", { hour12: false })
            : now();

        setMqttActive();

        /* IMEI: try payload first, then extract from topic */
        var imei = (payload && payload.imei) ? payload.imei : getImeiFromTopic(topic);
        if (imei) el["dev-imei"].textContent = imei;
    }

    /* ── WiFi panel ── */
    function updateWifi(data) {
        if (data.config) {
            if (data.config.ssid) el["dev-ssid"].textContent = data.config.ssid;
            if (data.config.channel !== undefined) el["dev-channel"].textContent = data.config.channel;
            if (data.config.macaddr) el["dev-mac"].textContent = data.config.macaddr;
        }

        var count = data.clients_num || 0;
        el["dev-clients-num"].textContent = count;
        el["client-count"].textContent = count;

        el["clients-body"].innerHTML = "";

        if (!data.clients_info || data.clients_info.length === 0) {
            var tr = document.createElement("tr");
            tr.className = "empty-row";
            tr.innerHTML = '<td colspan="4">No clients connected</td>';
            el["clients-body"].appendChild(tr);
            return;
        }

        data.clients_info.forEach(function (client, i) {
            var tr = document.createElement("tr");
            var name = client.device_name || client.hostname || client.name || "\u2014";
            if (name === "*") name = "Unknown Device";
            tr.innerHTML =
                "<td>" + (i + 1) + "</td>" +
                "<td>" + escapeHtml(client.mac_address || client.macaddr || "\u2014") + "</td>" +
                "<td>" + escapeHtml(client.ip_address || client.ip || "\u2014") + "</td>" +
                "<td>" + escapeHtml(name) + "</td>";
            el["clients-body"].appendChild(tr);
        });
    }

    /* ── Heartbeat panel ── */
    function updateHeartbeat(data) {
        if (data.battery !== undefined) el["dev-battery"].textContent = data.battery + "%";
        if (data.gsm !== undefined) el["dev-gsm"].textContent = data.gsm + "/31";

        /* GPS coordinates — device sends latitude/longitude, 0,0 = no fix */
        var lat = data.latitude;
        var lon = data.longitude;
        if (lat !== undefined && lon !== undefined) {
            if (lat === 0 && lon === 0) {
                el["dev-gps"].textContent = "No Fix";
            } else {
                el["dev-gps"].textContent = Number(lat).toFixed(5) + ", " + Number(lon).toFixed(5);
            }
        }

        if (data.speed !== undefined) el["dev-speed"].textContent = data.speed + " km/h";

        /* Device uses "satelites" (sic) */
        if (data.satelites !== undefined) el["dev-sats"].textContent = data.satelites;

        /* ACC is a string like "ACC ON" / "ACC OFF" */
        if (data.acc !== undefined) el["dev-acc"].textContent = data.acc;

        /* move is a boolean */
        if (data.move !== undefined) el["dev-movement"].textContent = data.move ? "Moving" : "Stationary";

        /* alarm is a boolean */
        if (data.alarm !== undefined) el["dev-alarm"].textContent = data.alarm ? "ACTIVE" : "None";
    }

    /* ── Login handler ── */
    function updateLogin(data) {
        if (data && data.imei) el["dev-imei"].textContent = data.imei;
        el["dev-status"].textContent = "Online";
    }

    /* ── RFID panel ── */
    function updateRfid(data, receivedAt) {
        /* Remove empty placeholder */
        var empty = el["rfid-body"].querySelector(".empty-row");
        if (empty) empty.remove();

        var tr = document.createElement("tr");
        var scanTime = data.time ? data.time.split(" ")[1] || data.time : "";
        var timeDisplay = scanTime || (receivedAt ? new Date(receivedAt).toLocaleTimeString("en-GB") : now());
        var statusText = data.status === 0 ? "OK" : (data.status !== undefined ? "Error" : "\u2014");

        tr.innerHTML =
            "<td>" + escapeHtml(timeDisplay) + "</td>" +
            "<td>" + escapeHtml(data.userID || data.user_id || "\u2014") + "</td>" +
            "<td>" + escapeHtml(data.stationID || data.station_id || "\u2014") + "</td>" +
            "<td>" + escapeHtml(statusText) + "</td>";

        el["rfid-body"].insertBefore(tr, el["rfid-body"].firstChild);

        while (el["rfid-body"].children.length > MAX_RFID) {
            el["rfid-body"].removeChild(el["rfid-body"].lastChild);
        }
    }

    /* ── Message log ── */
    function addLog(topic, payload) {
        var entry = document.createElement("div");
        entry.className = "log-entry";

        var typeBadge = getTopicType(topic);
        entry.innerHTML =
            '<span class="log-ts">' + now() + "</span>" +
            '<span class="log-badge log-badge-' + typeBadge + '">' + typeBadge + "</span>" +
            '<span class="log-topic">' + escapeHtml(topic) + "</span>" +
            '<span class="log-payload">' + escapeHtml(payload) + "</span>";

        el["log-entries"].insertBefore(entry, el["log-entries"].firstChild);

        while (el["log-entries"].children.length > MAX_LOG) {
            el["log-entries"].removeChild(el["log-entries"].lastChild);
        }
    }

    /* ── Handle incoming WebSocket message ── */
    function handleMessage(evt) {
        var msg;
        try {
            msg = JSON.parse(evt.data);
        } catch (e) {
            addLog("parse-error", evt.data);
            return;
        }

        /* Handle subscription list updates from server */
        if (msg.type === "sub_list") {
            renderActiveSubs(msg.active_subs || []);
            return;
        }

        var topic = msg.topic || "unknown";
        var rawPayload = msg.payload || "";
        var receivedAt = msg.receivedAt || null;

        addLog(topic, rawPayload);

        /* Parse payload (may be JSON string or already an object) */
        var payload;
        if (typeof rawPayload === "string") {
            try {
                payload = JSON.parse(rawPayload);
            } catch (e) {
                /* Not JSON — still update common info */
                updateCommon(topic, null, receivedAt);
                return;
            }
        } else if (typeof rawPayload === "object") {
            payload = rawPayload;
        } else {
            updateCommon(topic, null, receivedAt);
            return;
        }

        /* Always update common device info */
        updateCommon(topic, payload, receivedAt);

        /* Route to type-specific handler */
        var type = getTopicType(topic);
        if (type === "wifi") updateWifi(payload);
        if (type === "heartbeat") updateHeartbeat(payload);
        if (type === "rfid") updateRfid(payload, receivedAt);
        if (type === "login") updateLogin(payload);
    }

    /* ── Topic panel event handlers ── */
    el["btn-subscribe"].addEventListener("click", function () {
        subscribeTopic(el["topic-input"].value);
    });

    el["topic-input"].addEventListener("keydown", function (e) {
        if (e.key === "Enter") subscribeTopic(el["topic-input"].value);
    });

    /* Preset buttons */
    var presets = document.querySelectorAll(".preset[data-topic]");
    presets.forEach(function (btn) {
        btn.addEventListener("click", function () {
            var topic = btn.getAttribute("data-topic");
            el["topic-input"].value = topic;
            subscribeTopic(topic);
        });
    });

    /* ── WebSocket connection ── */
    function connect() {
        if (ws) {
            ws.onclose = null;
            ws.close();
        }

        var proto = location.protocol === "https:" ? "wss:" : "ws:";
        var url = proto + "//" + location.host + "/ws";

        ws = new WebSocket(url, "dashboard-ws");

        ws.onopen = function () {
            setConnected(true);
            requestSubList();
            if (reconnectTimer) {
                clearTimeout(reconnectTimer);
                reconnectTimer = null;
            }
        };

        ws.onclose = function () {
            setConnected(false);
            scheduleReconnect();
        };

        ws.onerror = function () {
            setConnected(false);
        };

        ws.onmessage = handleMessage;
    }

    function scheduleReconnect() {
        if (reconnectTimer) return;
        reconnectTimer = setTimeout(function () {
            reconnectTimer = null;
            connect();
        }, RECONNECT_MS);
    }

    connect();
})();
