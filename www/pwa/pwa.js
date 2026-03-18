/*
 * pwa.js — TransitTag PWA
 * Vanilla JS, IIFE, strict mode.
 * WebSocket client to the C server dashboard-ws protocol.
 */

(function () {
    "use strict";

    /* ── Constants ──────────────────────────────────────────────── */

    var RECONNECT_MS   = 3000;
    var SPLASH_MS      = 1200;    /* Minimum splash duration */
    var TOAST_DURATION = 3500;    /* ms before toast auto-hides */
    var PIN_CONDUCTOR  = "1234";  /* Default — real auth via WS response */
    var PIN_OWNER      = "5678";

    /* ── Application state ──────────────────────────────────────── */

    var state = {
        role:          null,    /* 'passenger' | 'conductor' | 'owner' */
        seats:         [],      /* Array of seat objects (cached) */
        mySeatId:      null,    /* Passenger's own seat id */
        fare:          50,
        route:         "CBD \u2192 Westlands",
        peakHour:      false,
        activeSeatId:  null,    /* Conductor action sheet target */
        paymentFlow:   null,    /* 'self' | 'push_them' | 'pay_for' */
        pinBuffer:     "",
        pinTarget:     null,    /* 'conductor' | 'owner' */
        summary:       {},
        connected:     false,
        authed:        false,   /* Whether PIN has been accepted for this session */
        stops:         [],
        destStopId:    null,
        distanceM:     0
    };

    /* ── Seat-from-URL helper ───────────────────────────────────── */

    function getSeatFromUrl() {
        var params = new URLSearchParams(window.location.search);
        var s = parseInt(params.get("seat"), 10);
        return (s > 0 && s <= 20) ? s : null;
    }

    /* ── Seat model ─────────────────────────────────────────────── */
    /*
     * Each seat: { id, status, phone, receipt, fare, paidBy }
     * status: 'empty' | 'seated' | 'paying' | 'paid_mpesa' |
     *         'paid_cash' | 'paid_neighbour' | 'overdue'
     */

    /* ── WebSocket ──────────────────────────────────────────────── */

    var ws            = null;
    var reconnectTimer = null;

    function connect() {
        if (ws) {
            ws.onclose = null;
            ws.onerror = null;
            try { ws.close(); } catch (e) { /* ignore */ }
            ws = null;
        }

        var proto = location.protocol === "https:" ? "wss:" : "ws:";
        var url   = proto + "//" + location.host + "/ws";

        try {
            ws = new WebSocket(url, "dashboard-ws");
        } catch (e) {
            setConnStatus("disconnected");
            scheduleReconnect();
            return;
        }

        ws.onopen = function () {
            state.connected = true;
            setConnStatus("connected");
            showToast("Connected", "success");

            if (reconnectTimer) {
                clearTimeout(reconnectTimer);
                reconnectTimer = null;
            }

            /* If already in a role, re-request state to refresh cache */
            if (state.role === "passenger" || state.role === "conductor") {
                sendCommand({ action: "get_seats" });
            }
            if (state.role === "conductor" || state.role === "owner") {
                sendCommand({ action: "get_summary" });
            }
        };

        ws.onclose = function () {
            state.connected = false;
            setConnStatus("disconnected");
            scheduleReconnect();
        };

        ws.onerror = function () {
            state.connected = false;
            setConnStatus("disconnected");
        };

        ws.onmessage = handleMessage;
    }

    function scheduleReconnect() {
        if (reconnectTimer) return;
        setConnStatus("reconnecting");
        reconnectTimer = setTimeout(function () {
            reconnectTimer = null;
            connect();
        }, RECONNECT_MS);
    }

    function sendCommand(obj) {
        if (ws && ws.readyState === WebSocket.OPEN) {
            try {
                ws.send(JSON.stringify(obj));
            } catch (e) {
                console.warn("[WS] send failed:", e);
            }
        }
    }

    /* ── Incoming message routing ───────────────────────────────── */

    function handleMessage(evt) {
        var msg;
        try {
            msg = JSON.parse(evt.data);
        } catch (e) {
            console.warn("[WS] unparseable message:", evt.data);
            return;
        }

        var t = msg.type || "";

        /* PWA-specific message types */
        if (t === "seats_state") {
            handleSeatsState(msg);
        } else if (t === "seat_update") {
            handleSeatUpdate(msg);
        } else if (t === "payment_pending") {
            handlePaymentPending(msg);
        } else if (t === "payment_confirmed") {
            handlePaymentConfirmed(msg);
        } else if (t === "day_summary") {
            handleDaySummary(msg);
        } else if (t === "auth_ok") {
            state.authed = true;
            enterRole(state.pinTarget);
        } else if (t === "auth_fail") {
            showPinError();
        } else if (t === "sub_list") {
            /* Dashboard compatibility — ignore in PWA */
        } else if (t === "heartbeat") {
            /* Use GPS/battery data for owner view even from MQTT heartbeats */
            if (state.role === "owner" && msg.payload) {
                updateOwnerVehicle(msg.payload);
            }
        } else if (t === "stops_list") {
            handleStopsList(msg);
        } else if (t === "fare_quote") {
            handleFareQuote(msg);
        } else if (t === "fare_error") {
            handleFareError(msg);
        } else {
            /* Legacy MQTT envelope: { topic, payload, receivedAt } */
            if (msg.topic) {
                handleLegacyEnvelope(msg);
            }
        }
    }

    /* ── seats_state: full seat array from server ───────────────── */

    function handleSeatsState(msg) {
        state.seats  = Array.isArray(msg.seats)  ? msg.seats  : state.seats;
        state.fare   = typeof msg.fare  === "number" ? msg.fare  : state.fare;
        state.route  = typeof msg.route === "string" ? msg.route : state.route;
        state.peakHour = !!msg.peak;

        if (state.role === "passenger") {
            var seat = detectMySeat(state.seats);
            renderPassengerView(seat);
        }
        if (state.role === "conductor") {
            renderSeatGrid(state.seats);
            updateConductorSummary(state.seats);
            updateConductorFareDisplay();
        }
    }

    /* ── seat_update: single seat changed ───────────────────────── */

    function handleSeatUpdate(msg) {
        if (!msg.seat) return;
        var updated = msg.seat;

        /* Merge into local seats cache */
        var found = false;
        for (var i = 0; i < state.seats.length; i++) {
            if (state.seats[i].id === updated.id) {
                state.seats[i] = updated;
                found = true;
                break;
            }
        }
        if (!found) state.seats.push(updated);

        if (state.role === "conductor") {
            updateSeatInGrid(updated);
            updateConductorSummary(state.seats);
        }
        if (state.role === "passenger" && updated.id === state.mySeatId) {
            renderPassengerView(updated);
        }
    }

    /* ── payment_pending ────────────────────────────────────────── */

    function handlePaymentPending(msg) {
        if (state.role === "passenger" && msg.seat_id === state.mySeatId) {
            showWaiting("Waiting for M-Pesa on " +
                (msg.phone ? formatPhoneDisplay(msg.phone) : "your phone") + "...");
        }
        showToast("M-Pesa STK Push sent", "info");
    }

    /* ── payment_confirmed ──────────────────────────────────────── */

    function handlePaymentConfirmed(msg) {
        if (state.role === "passenger" && msg.seat_id === state.mySeatId) {
            hideWaiting();
            showReceipt({
                receipt:    msg.receipt    || "",
                amount:     "KES " + (msg.amount || state.fare),
                method:     msg.method     || "M-Pesa",
                payer_phone: msg.payer_phone || ""
            });
            updateHero(getSeatById(msg.seat_id));
        }
        showToast("Payment confirmed \u2713", "success");
    }

    /* ── day_summary ────────────────────────────────────────────── */

    function handleDaySummary(msg) {
        state.summary = msg;
        if (state.role === "conductor") {
            updateConductorFromSummary(msg);
        }
        if (state.role === "owner") {
            updateOwnerDashboard(msg);
        }
    }

    /* ── Legacy MQTT envelope (heartbeat/wifi/etc.) ─────────────── */

    function handleLegacyEnvelope(msg) {
        var topic   = msg.topic   || "";
        var rawPay  = msg.payload || "";
        var payload;

        if (typeof rawPay === "string") {
            try { payload = JSON.parse(rawPay); } catch (e) { return; }
        } else {
            payload = rawPay;
        }

        if (!payload) return;

        /* Owner view: pull GPS / battery from heartbeat */
        if (state.role === "owner" && topic.indexOf("/heartbeat/") !== -1) {
            updateOwnerVehicle(payload);
        }
    }

    /* ── Screen management ──────────────────────────────────────── */

    var screenTitles = {
        "s-loading":   "TransitTag",
        "s-landing":   "TransitTag",
        "s-pin":       "PIN Entry",
        "s-passenger": "My Seat",
        "s-conductor": "Conductor",
        "s-owner":     "Owner Dashboard"
    };

    function showScreen(id) {
        var screens = document.querySelectorAll(".screen");
        for (var i = 0; i < screens.length; i++) {
            screens[i].hidden = true;
            screens[i].classList.remove("screen-active");
        }
        var target = document.getElementById(id);
        if (target) {
            target.hidden = false;
            target.classList.add("screen-active");
        }

        var titleEl = document.getElementById("hdr-title");
        if (titleEl) titleEl.textContent = screenTitles[id] || "TransitTag";

        /* Show back button except on landing/loading */
        var backBtn = document.getElementById("btn-back");
        if (backBtn) {
            backBtn.hidden = (id === "s-landing" || id === "s-loading");
        }
    }

    /* ── Role selection ─────────────────────────────────────────── */

    function selectRole(role) {
        if (role === "passenger") {
            state.role = "passenger";
            state.mySeatId = null;
            showScreen("s-passenger");
            /* Reset passenger view */
            updateHero(null);
            hideCard("pax-fare-card");
            hideCard("pax-payment-card");
            hideCard("pax-phone-card");
            hideCard("pax-waiting-card");
            hideCard("pax-receipt-card");
            hideCard("pax-no-seat");

            /* If ?seat=N is in URL, pre-select that seat */
            var urlSeat = getSeatFromUrl();
            if (urlSeat) {
                state.mySeatId = urlSeat;
                /* Show the hero with the seat number immediately */
                updateHero({ id: urlSeat, status: "seated" });
                hideCard("pax-no-seat");
            }

            /* Request current seats and route stops */
            sendCommand({ action: "get_seats" });
            sendCommand({ action: "get_stops" });
        } else if (role === "conductor" || role === "owner") {
            state.pinTarget = role;
            state.pinBuffer = "";
            renderPinDots();
            hideEl("pin-error");
            var subtitle = document.getElementById("pin-subtitle");
            if (subtitle) {
                subtitle.textContent = role === "conductor"
                    ? "Conductor access" : "Owner / SACCO access";
            }
            showScreen("s-pin");
        }
    }

    /* ── Enter role (after PIN accepted) ───────────────────────── */

    function enterRole(role) {
        state.role = role;
        if (role === "conductor") {
            showScreen("s-conductor");
            sendCommand({ action: "get_seats" });
            sendCommand({ action: "get_summary" });
        } else if (role === "owner") {
            showScreen("s-owner");
            sendCommand({ action: "get_summary" });
        }
    }

    /* ── PIN pad logic ──────────────────────────────────────────── */

    function pinKeyPress(key) {
        if (key === "del") {
            state.pinBuffer = state.pinBuffer.slice(0, -1);
            renderPinDots();
            hideEl("pin-error");
        } else if (key === "ok") {
            submitPin();
        } else if (state.pinBuffer.length < 4) {
            state.pinBuffer += key;
            renderPinDots();
            if (state.pinBuffer.length === 4) {
                /* Auto-submit on 4th digit for speed */
                submitPin();
            }
        }
    }

    function renderPinDots() {
        var dots = document.querySelectorAll(".pin-dot");
        for (var i = 0; i < dots.length; i++) {
            if (i < state.pinBuffer.length) {
                dots[i].classList.add("filled");
            } else {
                dots[i].classList.remove("filled");
            }
        }
    }

    function submitPin() {
        if (state.pinBuffer.length < 4) {
            showToast("Enter all 4 digits", "error");
            return;
        }

        var expected = state.pinTarget === "conductor" ? PIN_CONDUCTOR : PIN_OWNER;

        /* Try local check first (offline fallback); also send to server for audit */
        sendCommand({
            action: "auth",
            role:   state.pinTarget,
            pin:    state.pinBuffer
        });

        /* Local validation as fallback when server hasn't responded */
        if (state.pinBuffer === expected) {
            state.authed = true;
            enterRole(state.pinTarget);
        } else {
            showPinError();
        }

        state.pinBuffer = "";
        renderPinDots();
    }

    function showPinError() {
        showEl("pin-error");
        /* Shake animation on pad */
        var pad = document.querySelector(".pin-pad");
        if (pad) {
            pad.classList.remove("shake");
            /* Force reflow to restart animation */
            void pad.offsetWidth;
            pad.classList.add("shake");
        }
        state.pinBuffer = "";
        renderPinDots();
        showToast("Incorrect PIN", "error");
    }

    /* ── Passenger view rendering ───────────────────────────────── */

    function detectMySeat(seats) {
        if (!seats || seats.length === 0) return null;

        /* If we already know our seat, find it */
        if (state.mySeatId) {
            return getSeatById(state.mySeatId) || null;
        }

        /* Find first seated/paying/paid seat as best guess */
        for (var i = 0; i < seats.length; i++) {
            var s = seats[i];
            if (s.status === "seated" || s.status === "paying" ||
                s.status === "paid_mpesa" || s.status === "paid_cash" ||
                s.status === "paid_neighbour") {
                state.mySeatId = s.id;
                return s;
            }
        }
        return null;
    }

    function renderPassengerView(seat) {
        if (!seat) {
            /* No seat assigned yet */
            updateHero(null);
            hideCard("pax-fare-card");
            hideCard("pax-payment-card");
            hideCard("pax-phone-card");
            hideCard("pax-waiting-card");
            hideCard("pax-receipt-card");
            showCard("pax-no-seat");
            return;
        }

        hideCard("pax-no-seat");
        updateHero(seat);

        /* Fare card — always show once we have a seat */
        var routeEl  = document.getElementById("pax-route");
        var amtEl    = document.getElementById("pax-amount");
        var peakEl   = document.getElementById("pax-peak-badge");
        if (routeEl)  routeEl.textContent  = state.route;
        if (amtEl)    amtEl.textContent    = "KES " + (seat.fare || state.fare);
        if (peakEl)   peakEl.hidden        = !state.peakHour;
        showCard("pax-fare-card");

        /* Show payment options only when seat is occupied and unpaid */
        if (seat.status === "seated" || seat.status === "paying") {
            if (seat.status === "seated") {
                showCard("pax-payment-card");
                hideCard("pax-waiting-card");
            } else {
                hideCard("pax-payment-card");
                showWaiting("Waiting for M-Pesa confirmation...");
            }
            hideCard("pax-receipt-card");
        } else if (seat.status === "paid_mpesa" || seat.status === "paid_cash" ||
                   seat.status === "paid_neighbour") {
            hideCard("pax-payment-card");
            hideCard("pax-waiting-card");
            showReceipt({
                receipt:     seat.receipt  || "",
                amount:      "KES " + (seat.fare || state.fare),
                method:      seat.status === "paid_cash" ? "Cash" : "M-Pesa",
                payer_phone: seat.paidBy  || ""
            });
        } else if (seat.status === "overdue") {
            showCard("pax-payment-card");
            hideCard("pax-waiting-card");
            hideCard("pax-receipt-card");
            showToast("Please pay your fare — overdue!", "error");
        } else {
            /* empty or unknown */
            hideCard("pax-payment-card");
            hideCard("pax-waiting-card");
            hideCard("pax-receipt-card");
        }
    }

    function updateHero(seat) {
        var heroEl    = document.getElementById("pax-hero");
        var numEl     = document.getElementById("pax-seat-num");
        var statusEl  = document.getElementById("pax-seat-status");
        if (!heroEl || !numEl || !statusEl) return;

        /* Remove all status classes */
        heroEl.className = "seat-hero";

        if (!seat) {
            heroEl.classList.add("seat-hero-empty");
            numEl.textContent    = "?";
            statusEl.textContent = "Scanning seats...";
            return;
        }

        numEl.textContent    = seat.id;
        statusEl.textContent = seatStatusLabel(seat.status);
        heroEl.classList.add("seat-hero-" + statusCssKey(seat.status));
    }

    /* ── Payment flows (passenger) ──────────────────────────────── */

    function showPhoneInput(flow, seatId) {
        state.paymentFlow  = flow;
        state.activeSeatId = seatId || state.mySeatId;

        var titleEl  = document.getElementById("pax-phone-title");
        var descEl   = document.getElementById("pax-phone-desc");
        var inputEl  = document.getElementById("pax-phone-input");

        var titles = {
            "self":     "Enter your M-Pesa number",
            "push_them": "Enter your number — STK Push fires to you",
            "pay_for":  "Enter the payer's M-Pesa number"
        };
        var descs = {
            "self":     "You will receive a push prompt on your phone",
            "push_them": "The neighbour enters this; the seated person pays",
            "pay_for":  "The payer's phone will receive the STK Push"
        };

        if (titleEl)  titleEl.textContent = titles[flow] || "Enter phone number";
        if (descEl)   descEl.textContent  = descs[flow]  || "07xx xxx xxx";
        if (inputEl)  inputEl.value       = "";

        hideCard("pax-payment-card");
        showCard("pax-phone-card");
        if (inputEl) inputEl.focus();
    }

    function confirmPhoneInput() {
        var inputEl = document.getElementById("pax-phone-input");
        if (!inputEl) return;
        var phone = formatPhone(inputEl.value);
        if (!phone) {
            showToast("Enter a valid 9-digit number", "error");
            return;
        }

        var seatId = state.activeSeatId || state.mySeatId;

        if (state.paymentFlow === "self") {
            sendCommand({ action: "pay_self", seat_id: seatId, phone: phone });
        } else if (state.paymentFlow === "push_them") {
            sendCommand({ action: "pay_push_them", seat_id: seatId, their_phone: phone });
        } else if (state.paymentFlow === "pay_for") {
            sendCommand({ action: "pay_for_seat", seat_id: seatId, payer_phone: phone });
        }

        hideCard("pax-phone-card");
        showWaiting("Sending M-Pesa request...");
    }

    function showWaiting(msg) {
        var msgEl = document.getElementById("pax-waiting-msg");
        if (msgEl) msgEl.textContent = msg || "Waiting...";
        showCard("pax-waiting-card");
    }

    function hideWaiting() {
        hideCard("pax-waiting-card");
    }

    function showReceipt(data) {
        var idEl     = document.getElementById("rcpt-id");
        var amtEl    = document.getElementById("rcpt-amount");
        var methEl   = document.getElementById("rcpt-method");
        var phoneEl  = document.getElementById("rcpt-phone");

        if (idEl)    idEl.textContent    = data.receipt    || "\u2014";
        if (amtEl)   amtEl.textContent   = data.amount     || "\u2014";
        if (methEl)  methEl.textContent  = data.method     || "\u2014";
        if (phoneEl) phoneEl.textContent = data.payer_phone
                                           ? formatPhoneDisplay(data.payer_phone)
                                           : "\u2014";

        var qrImg = document.getElementById("receipt-qr");
        if (qrImg && data.receipt) {
            qrImg.src = "/api/qr?data=" + encodeURIComponent(data.receipt);
            qrImg.removeAttribute("hidden");
        }

        showCard("pax-receipt-card");
    }

    /* ── Conductor: seat grid ───────────────────────────────────── */

    function renderSeatGrid(seats) {
        var grid = document.getElementById("cond-seat-grid");
        if (!grid) return;

        /* Clear and rebuild */
        grid.innerHTML = "";

        for (var i = 0; i < seats.length; i++) {
            grid.appendChild(buildSeatEl(seats[i]));
        }

        /* Update evasion alerts */
        renderEvasionAlerts(seats);
    }

    function buildSeatEl(seat) {
        var el = document.createElement("div");
        el.className   = "seat-item " + seatStatusClass(seat.status);
        el.setAttribute("data-seat-id", seat.id);

        var numSpan  = document.createElement("span");
        numSpan.className   = "seat-num";
        numSpan.textContent = seat.id;

        var iconSpan = document.createElement("span");
        iconSpan.className   = "seat-icon";
        iconSpan.textContent = seatStatusIcon(seat.status);

        el.appendChild(numSpan);
        el.appendChild(iconSpan);

        /* Cash badge */
        if (seat.status === "paid_cash") {
            var badge = document.createElement("span");
            badge.className   = "cash-badge";
            badge.textContent = "C";
            el.appendChild(badge);
        }

        el.addEventListener("click", function () {
            showActionSheet(parseInt(el.getAttribute("data-seat-id"), 10));
        });

        return el;
    }

    function updateSeatInGrid(seat) {
        var el = document.querySelector("[data-seat-id='" + seat.id + "']");
        if (!el) {
            /* Seat not in grid yet — full rebuild */
            renderSeatGrid(state.seats);
            return;
        }

        /* Swap classes */
        el.className = "seat-item " + seatStatusClass(seat.status);

        /* Update icon */
        var iconEl = el.querySelector(".seat-icon");
        if (iconEl) iconEl.textContent = seatStatusIcon(seat.status);

        /* Cash badge */
        var existBadge = el.querySelector(".cash-badge");
        if (seat.status === "paid_cash") {
            if (!existBadge) {
                var badge = document.createElement("span");
                badge.className   = "cash-badge";
                badge.textContent = "C";
                el.appendChild(badge);
            }
        } else {
            if (existBadge) existBadge.remove();
        }
    }

    function renderEvasionAlerts(seats) {
        var list = document.getElementById("cond-evasion-list");
        if (!list) return;

        var overdue = [];
        for (var i = 0; i < seats.length; i++) {
            if (seats[i].status === "overdue") overdue.push(seats[i]);
        }

        list.innerHTML = "";
        if (overdue.length === 0) {
            list.innerHTML = '<p class="muted-text">No alerts right now.</p>';
            return;
        }

        for (var j = 0; j < overdue.length; j++) {
            var s   = overdue[j];
            var item = document.createElement("div");
            item.className   = "evasion-alert-item";
            item.textContent = "\u26a0 Seat " + s.id + " overdue — KES " + (s.fare || state.fare);
            list.appendChild(item);
        }
    }

    /* ── Conductor: summary bar ─────────────────────────────────── */

    function updateConductorSummary(seats) {
        var paid   = 0;
        var seated = 0;
        var revenue = 0;
        var unpaid = 0;

        for (var i = 0; i < seats.length; i++) {
            var s = seats[i];
            if (s.status === "seated" || s.status === "paying" || s.status === "overdue") {
                seated++;
            }
            if (s.status === "paid_mpesa" || s.status === "paid_cash" ||
                s.status === "paid_neighbour") {
                paid++;
                seated++;
                revenue += (s.fare || state.fare);
            }
            if (s.status === "overdue") {
                unpaid++;
            }
        }

        setEl("cond-paid",    paid);
        setEl("cond-seated",  seated);
        setEl("cond-revenue", revenue);
        setEl("cond-unpaid",  unpaid);
    }

    function updateConductorFromSummary(msg) {
        /* Supplement grid summary with server-side totals */
        if (msg.revenue_total !== undefined) {
            setEl("cond-revenue", msg.revenue_total);
        }
        if (msg.evaders !== undefined) {
            setEl("cond-unpaid", msg.evaders);
        }
    }

    function updateConductorFareDisplay() {
        var routeInput = document.getElementById("cond-route-input");
        var fareInput  = document.getElementById("cond-fare-input");
        var peakEl     = document.getElementById("cond-peak-indicator");
        if (routeInput) routeInput.value  = state.route;
        if (fareInput)  fareInput.value   = state.fare;
        if (peakEl) {
            if (state.peakHour) {
                peakEl.className   = "peak-indicator peak-on";
                peakEl.textContent = "PEAK HOUR";
            } else {
                peakEl.className   = "peak-indicator peak-off";
                peakEl.textContent = "OFF-PEAK";
            }
        }
    }

    /* ── Conductor: action sheet ────────────────────────────────── */

    function showActionSheet(seatId) {
        state.activeSeatId = seatId;
        var seat = getSeatById(seatId);

        var titleEl = document.getElementById("action-sheet-title");
        if (titleEl) {
            titleEl.textContent = "Seat " + seatId +
                (seat ? " \u2014 " + seatStatusLabel(seat.status) : "");
        }

        var inputEl = document.getElementById("as-phone-input");
        if (inputEl) inputEl.value = (seat && seat.phone) ? seat.phone : "";

        showEl("overlay");
        showEl("action-sheet");
    }

    function hideActionSheet() {
        hideEl("action-sheet");
        hideEl("overlay");
        state.activeSeatId = null;
    }

    /* ── Owner dashboard ────────────────────────────────────────── */

    function updateOwnerDashboard(msg) {
        /* Revenue hero */
        setEl("own-revenue", "KES " + (msg.revenue_total || 0));
        setEl("own-trips",   (msg.trips || 0) + " trips");
        setEl("own-pax",     (msg.total_passengers || 0) + " passengers");

        /* Payment split bars */
        var total = (msg.total_passengers || 0);
        if (total > 0) {
            var mpesaPct   = Math.round(((msg.paid_mpesa || 0) / total) * 100);
            var cashPct    = Math.round(((msg.paid_cash  || 0) / total) * 100);
            var evadedPct  = Math.round(((msg.evaders    || 0) / total) * 100);
            setSplitBar("own-mpesa",  mpesaPct);
            setSplitBar("own-cash",   cashPct);
            setSplitBar("own-evaded", evadedPct);
        } else {
            setSplitBar("own-mpesa",  0);
            setSplitBar("own-cash",   0);
            setSplitBar("own-evaded", 0);
        }

        /* GPS/speed/battery/GSM — these come from heartbeats */
        updateOwnerVehicle({
            latitude:  msg.lat     || 0,
            longitude: msg.lon     || 0,
            speed:     msg.speed   || 0,
            battery:   msg.battery || 0,
            gsm:       msg.gsm     || 0
        });

        /* Evasion stats */
        var evasionLoss = (msg.evasion_loss || (msg.evaders || 0) * (state.fare || 50));
        var evasionRate = total > 0
            ? Math.round(((msg.evaders || 0) / total) * 100) : 0;
        setEl("own-evaders-count",  msg.evaders    || 0);
        setEl("own-evasion-loss",   "KES " + evasionLoss);
        setEl("own-evasion-rate",   evasionRate + "%");
    }

    function updateOwnerVehicle(data) {
        var lat = data.latitude  || data.lat || 0;
        var lon = data.longitude || data.lon || 0;
        var gpsText = (lat === 0 && lon === 0)
            ? "No fix"
            : lat.toFixed(5) + ", " + lon.toFixed(5);

        setEl("own-gps",     gpsText);
        setEl("own-speed",   (data.speed   || 0) + " km/h");
        setEl("own-battery", (data.battery || 0) + "%");
        setEl("own-gsm",     (data.gsm     || 0) + "/31");
    }

    function setSplitBar(prefix, pct) {
        var barEl = document.getElementById(prefix + "-bar");
        var pctEl = document.getElementById(prefix + "-pct");
        if (barEl) barEl.style.width = Math.min(100, Math.max(0, pct)) + "%";
        if (pctEl) pctEl.textContent = pct + "%";
    }

    /* ── Connection status display ──────────────────────────────── */

    function setConnStatus(status) {
        var banner   = document.getElementById("conn-banner");
        var label    = document.getElementById("conn-label");
        if (!banner || !label) return;

        banner.className = "conn-banner conn-" + status;
        var labels = {
            "connected":     "LIVE",
            "disconnected":  "OFFLINE",
            "reconnecting":  "RECONN"
        };
        label.textContent = labels[status] || "CONN";
    }

    /* ── Toast notifications ────────────────────────────────────── */

    var toastTimer = null;

    function showToast(msg, type) {
        var el = document.getElementById("toast");
        if (!el) return;

        el.textContent  = msg;
        el.className    = "toast toast-" + (type || "info");
        el.hidden       = false;
        el.classList.add("toast-show");
        el.classList.remove("toast-hide");

        if (toastTimer) clearTimeout(toastTimer);
        toastTimer = setTimeout(function () {
            el.classList.remove("toast-show");
            el.classList.add("toast-hide");
            setTimeout(function () {
                el.hidden = true;
                el.classList.remove("toast-hide");
            }, 350);
        }, TOAST_DURATION);
    }

    /* ── Helpers ────────────────────────────────────────────────── */

    function getSeatById(id) {
        for (var i = 0; i < state.seats.length; i++) {
            if (state.seats[i].id === id) return state.seats[i];
        }
        return null;
    }

    function formatPhone(input) {
        /* Strip +254, leading 0, spaces, hyphens → 9 digit string */
        if (!input) return "";
        var s = String(input).replace(/[\s\-\(\)]/g, "");
        if (s.startsWith("+254")) s = s.slice(4);
        if (s.startsWith("254"))  s = s.slice(3);
        if (s.startsWith("0"))    s = s.slice(1);
        if (/^\d{9}$/.test(s)) return s;
        return "";
    }

    function formatPhoneDisplay(phone) {
        /* 712345678 → 0712 345 678 */
        var s = String(phone);
        if (s.length === 9) {
            return "0" + s.slice(0, 3) + " " + s.slice(3, 6) + " " + s.slice(6);
        }
        return phone;
    }

    function seatStatusLabel(status) {
        var labels = {
            "empty":          "Empty",
            "seated":         "Pending payment",
            "paying":         "Awaiting M-Pesa",
            "paid_mpesa":     "Paid (M-Pesa)",
            "paid_cash":      "Paid (Cash)",
            "paid_neighbour": "Paid (Neighbour)",
            "overdue":        "OVERDUE"
        };
        return labels[status] || status || "Unknown";
    }

    function seatStatusIcon(status) {
        var icons = {
            "empty":          "",
            "seated":         "\u23f3",   /* ⏳ */
            "paying":         "\u23f3",
            "paid_mpesa":     "\u2713",   /* ✓ */
            "paid_cash":      "\u2713",
            "paid_neighbour": "\u2713",
            "overdue":        "\u2717"    /* ✗ */
        };
        return icons[status] || "";
    }

    function seatStatusClass(status) {
        var map = {
            "empty":          "seat-empty",
            "seated":         "seat-seated",
            "paying":         "seat-paying",
            "paid_mpesa":     "seat-paid-mpesa",
            "paid_cash":      "seat-paid-cash",
            "paid_neighbour": "seat-paid-neighbour",
            "overdue":        "seat-overdue"
        };
        return map[status] || "seat-empty";
    }

    function statusCssKey(status) {
        /* Convert 'paid_mpesa' → 'paid-mpesa' for CSS class names */
        return (status || "empty").replace(/_/g, "-");
    }

    function setEl(id, val) {
        var el = document.getElementById(id);
        if (el) el.textContent = String(val);
    }

    function showEl(id) {
        var el = document.getElementById(id);
        if (el) el.hidden = false;
    }

    function hideEl(id) {
        var el = document.getElementById(id);
        if (el) el.hidden = true;
    }

    function showCard(id) { showEl(id); }
    function hideCard(id) { hideEl(id); }

    /* ── Event listeners ────────────────────────────────────────── */

    function bindEvents() {
        /* Role cards */
        var roleCards = document.querySelectorAll(".role-card[data-role]");
        for (var i = 0; i < roleCards.length; i++) {
            roleCards[i].addEventListener("click", function (e) {
                var card = e.currentTarget;
                selectRole(card.getAttribute("data-role"));
            });
        }

        /* PIN keys */
        var pinKeys = document.querySelectorAll(".pin-key[data-key]");
        for (var j = 0; j < pinKeys.length; j++) {
            pinKeys[j].addEventListener("click", function (e) {
                pinKeyPress(e.currentTarget.getAttribute("data-key"));
            });
        }

        /* Back button */
        var backBtn = document.getElementById("btn-back");
        if (backBtn) {
            backBtn.addEventListener("click", function () {
                if (state.role === "passenger") {
                    /* If phone input is showing, go back to payment options */
                    var phoneCard = document.getElementById("pax-phone-card");
                    if (phoneCard && !phoneCard.hidden) {
                        hideCard("pax-phone-card");
                        showCard("pax-payment-card");
                        return;
                    }
                }
                /* Default: back to landing */
                state.role      = null;
                state.mySeatId  = null;
                state.pinTarget = null;
                state.pinBuffer = "";
                showScreen("s-landing");
            });
        }

        /* Passenger: payment buttons */
        bindPassengerPayButtons();

        /* Passenger: phone input confirm/cancel */
        var confirmBtn = document.getElementById("btn-phone-confirm");
        if (confirmBtn) confirmBtn.addEventListener("click", confirmPhoneInput);

        var cancelBtn = document.getElementById("btn-phone-cancel");
        if (cancelBtn) cancelBtn.addEventListener("click", function () {
            hideCard("pax-phone-card");
            showCard("pax-payment-card");
        });

        /* Passenger: phone input enter key */
        var phoneInput = document.getElementById("pax-phone-input");
        if (phoneInput) {
            phoneInput.addEventListener("keydown", function (e) {
                if (e.key === "Enter") confirmPhoneInput();
            });
        }

        /* Conductor: Set fare button */
        var setFareBtn = document.getElementById("btn-set-fare");
        if (setFareBtn) {
            setFareBtn.addEventListener("click", function () {
                var routeInput = document.getElementById("cond-route-input");
                var fareInput  = document.getElementById("cond-fare-input");
                var route = routeInput ? routeInput.value.trim() : state.route;
                var fare  = fareInput  ? parseInt(fareInput.value, 10) : state.fare;
                if (!route) { showToast("Enter a route", "error"); return; }
                if (isNaN(fare) || fare < 1) { showToast("Enter a valid fare", "error"); return; }
                state.route = route;
                state.fare  = fare;
                sendCommand({ action: "set_fare", fare: fare, route: route });
                showToast("Fare updated \u2713", "success");
            });
        }

        /* Action sheet buttons */
        bindActionSheetEvents();

        /* Overlay click to close action sheet */
        var overlay = document.getElementById("overlay");
        if (overlay) overlay.addEventListener("click", hideActionSheet);

    }

    function bindPassengerPayButtons() {
        var btnSelf = document.getElementById("btn-pay-self");
        if (btnSelf) {
            btnSelf.addEventListener("click", function () {
                showPhoneInput("self", state.mySeatId);
            });
        }

        var btnPushThem = document.getElementById("btn-pay-push-them");
        if (btnPushThem) {
            btnPushThem.addEventListener("click", function () {
                showPhoneInput("push_them", state.mySeatId);
            });
        }

        var btnPayForNeighbour = document.getElementById("btn-pay-for-neighbour");
        if (btnPayForNeighbour) {
            btnPayForNeighbour.addEventListener("click", function () {
                /* For "pay for neighbour" the passenger first picks a seat,
                 * then provides their own number as the payer.
                 * Simplified: use activeSeatId if set, else prompt */
                showPhoneInput("pay_for", state.mySeatId);
            });
        }

        var btnCash = document.getElementById("btn-pay-cash");
        if (btnCash) {
            btnCash.addEventListener("click", function () {
                showToast("Hand payment to the conductor", "info");
            });
        }
    }

    function bindActionSheetEvents() {
        /* Close button */
        var closeBtn = document.getElementById("as-btn-close");
        if (closeBtn) closeBtn.addEventListener("click", hideActionSheet);

        /* Pay self */
        var asSelf = document.getElementById("as-btn-pay-self");
        if (asSelf) {
            asSelf.addEventListener("click", function () {
                var phone = getActionSheetPhone();
                if (!phone) return;
                sendCommand({ action: "pay_self", seat_id: state.activeSeatId, phone: phone });
                showToast("STK Push sent to " + formatPhoneDisplay(phone), "info");
                hideActionSheet();
            });
        }

        /* Push them */
        var asPushThem = document.getElementById("as-btn-push-them");
        if (asPushThem) {
            asPushThem.addEventListener("click", function () {
                var phone = getActionSheetPhone();
                if (!phone) return;
                sendCommand({ action: "pay_push_them", seat_id: state.activeSeatId, their_phone: phone });
                showToast("Neighbour push initiated", "info");
                hideActionSheet();
            });
        }

        /* Pay for seat */
        var asPayFor = document.getElementById("as-btn-pay-for");
        if (asPayFor) {
            asPayFor.addEventListener("click", function () {
                var phone = getActionSheetPhone();
                if (!phone) return;
                sendCommand({ action: "pay_for_seat", seat_id: state.activeSeatId, payer_phone: phone });
                showToast("Third-party payment initiated", "info");
                hideActionSheet();
            });
        }

        /* Cash paid */
        var asCash = document.getElementById("as-btn-cash");
        if (asCash) {
            asCash.addEventListener("click", function () {
                sendCommand({ action: "cash_paid", seat_id: state.activeSeatId });
                showToast("Seat " + state.activeSeatId + " marked as CASH PAID", "success");
                hideActionSheet();
            });
        }

        /* Reset seat */
        var asReset = document.getElementById("as-btn-reset");
        if (asReset) {
            asReset.addEventListener("click", function () {
                sendCommand({ action: "reset_seat", seat_id: state.activeSeatId });
                showToast("Seat " + state.activeSeatId + " reset", "info");
                hideActionSheet();
            });
        }
    }

    function getActionSheetPhone() {
        var inputEl = document.getElementById("as-phone-input");
        var phone   = inputEl ? formatPhone(inputEl.value) : "";
        if (!phone) {
            showToast("Enter a valid phone number", "error");
            return null;
        }
        return phone;
    }

    /* ── Service Worker registration ────────────────────────────── */

    function registerSW() {
        if ("serviceWorker" in navigator) {
            navigator.serviceWorker.register("/sw.js").then(function (reg) {
                console.log("[SW] Registered, scope:", reg.scope);
            }).catch(function (err) {
                console.warn("[SW] Registration failed:", err);
            });
        }
    }

    /* ── App init ───────────────────────────────────────────────── */

    function init() {
        bindEvents();
        registerSW();

        /* Start WebSocket early */
        connect();

        /* If ?seat=N is in URL, auto-select passenger role after splash */
        var urlSeat = getSeatFromUrl();
        if (urlSeat) {
            setTimeout(function () {
                selectRole("passenger");
            }, SPLASH_MS);
        } else {
            /* Show landing after splash */
            setTimeout(function () {
                showScreen("s-landing");
            }, SPLASH_MS);
        }
    }

    /* Wire up on DOM ready */
    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", init);
    } else {
        init();
    }

    /* ── Stop picker ────────────────────────────────────────────── */

    function handleStopsList(msg) {
        state.stops = msg.stops || [];
        renderStopPicker();
    }

    function renderStopPicker() {
        var list = document.getElementById("pax-stop-list");
        if (!list) return;
        list.innerHTML = "";
        if (!state.stops.length) {
            list.innerHTML = '<div class="stop-loading">No stops configured</div>';
            return;
        }
        state.stops.forEach(function(stop) {
            var item = document.createElement("div");
            item.className = "stop-item";
            var dist = estimateKm(state.stops[0], stop);
            var fare = distToFare(dist * 1000);
            item.innerHTML =
                '<span class="stop-name">' + escapeHtml(stop.name) + '</span>' +
                '<span class="stop-fare">KES ' + fare + '</span>';
            item.addEventListener("click", function() { selectStop(stop.id); });
            list.appendChild(item);
        });
        /* show stop card if passenger has a seat but no destination yet */
        if (state.mySeatId !== null && !state.destStopId) {
            document.getElementById("pax-stop-card").removeAttribute("hidden");
        }
    }

    function selectStop(stopId) {
        sendCommand({ action: "select_stop", stop_id: stopId, seat_id: state.mySeatId });
        showToast("Getting fare...", "info");
    }

    function handleFareQuote(msg) {
        state.destStopId = msg.stop_id;
        state.distanceM  = msg.distance_m;
        state.fare       = msg.effective_fare;
        /* hide stop picker, show fare with distance */
        var sc = document.getElementById("pax-stop-card");
        if (sc) sc.setAttribute("hidden", "");
        var fd = document.getElementById("pax-distance");
        if (fd) {
            fd.textContent = (msg.distance_m / 1000).toFixed(1) + " km road distance";
            fd.removeAttribute("hidden");
        }
        /* update fare display */
        var el = document.getElementById("pax-amount");
        if (el) el.textContent = "KES " + msg.effective_fare;
        var routeEl = document.getElementById("pax-route");
        if (routeEl && msg.stop_name) routeEl.textContent += " \u2192 " + msg.stop_name;
        showToast("Fare: KES " + msg.effective_fare, "success");
    }

    function handleFareError(msg) {
        void msg;
        showToast("Could not calculate fare \u2014 using flat rate", "error");
    }

    /* Rough straight-line km estimate for fare preview before API responds */
    function estimateKm(from, to) {
        if (!from || !to) return 0;
        var dlat = (to.lat - from.lat) * 111;
        var dlon = (to.lon - from.lon) * 111 * Math.cos(from.lat * Math.PI / 180);
        return Math.sqrt(dlat*dlat + dlon*dlon);
    }

    /* Client-side fare tier mirror (matches routes_distance_to_base_fare in C) */
    function distToFare(dist_m) {
        if (dist_m <  2000) return 30;
        if (dist_m <  5000) return 50;
        if (dist_m < 10000) return 70;
        if (dist_m < 20000) return 100;
        return 130;
    }

    /* Simple HTML entity escaping */
    function escapeHtml(str) {
        return String(str)
            .replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;");
    }

})();
