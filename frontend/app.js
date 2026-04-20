/**
 * Intruder Detection System – Frontend JavaScript
 *
 * Polls /status every 500 ms and /history every 2 s.
 * Updates the dashboard UI in real-time without a page reload.
 */

"use strict";

// ── Configuration ────────────────────────────────────────────────
const STATUS_POLL_MS  = 500;   // status endpoint poll interval
const HISTORY_POLL_MS = 2000;  // history endpoint poll interval
const MAX_HISTORY_ROWS = 30;   // table rows to display

// Servo angle mapping
const SERVO_ANGLES = {
  LEFT:   30,
  CENTER: 90,
  RIGHT:  150,
  NONE:   90,
};

// State metadata
const STATE_META = {
  IDLE:     { icon: "🟢", label: "IDLE"     },
  DETECTED: { icon: "🟡", label: "DETECTED" },
  TRACKING: { icon: "🔵", label: "TRACKING" },
  ALERT:    { icon: "🔴", label: "ALERT"    },
};

// ── Element references ───────────────────────────────────────────
const elBadge       = document.getElementById("connection-badge");
const elStatusCard  = document.getElementById("status-card");
const elStateIcon   = document.getElementById("state-icon");
const elStateLabel  = document.getElementById("state-label");
const elLastUpdated = document.getElementById("last-updated-time");

const elPosLeft    = document.getElementById("pos-left");
const elPosCenter  = document.getElementById("pos-center");
const elPosRight   = document.getElementById("pos-right");
const elDistLeft   = document.getElementById("dist-left");
const elDistCenter = document.getElementById("dist-center");
const elDistRight  = document.getElementById("dist-right");

const elServoNeedle  = document.getElementById("servo-needle");
const elHistoryBody  = document.getElementById("history-body");

// ── Helper: format distance ───────────────────────────────────────
function fmtDist(val) {
  if (val === null || val === undefined) return "—";
  return Number(val).toFixed(1) + " cm";
}

// ── Helper: format ISO timestamp to local time ────────────────────
function fmtTime(isoStr) {
  if (!isoStr) return "—";
  try {
    return new Date(isoStr).toLocaleTimeString();
  } catch {
    return isoStr;
  }
}

// ── Update status card ────────────────────────────────────────────
function updateStatusCard(data) {
  const state = (data.state || "IDLE").toUpperCase();
  const meta  = STATE_META[state] || STATE_META.IDLE;

  // Remove all previous state classes from the card
  elStatusCard.classList.remove(
    "state-IDLE", "state-DETECTED", "state-TRACKING", "state-ALERT",
    "alert-active"
  );
  elStatusCard.classList.add("state-" + state);
  if (state === "ALERT") elStatusCard.classList.add("alert-active");

  elStateIcon.textContent  = "";          // CSS ::after injects the emoji
  elStateLabel.textContent = meta.label;
  elLastUpdated.textContent = fmtTime(data.timestamp);
}

// ── Update position cells ─────────────────────────────────────────
function updatePositionCard(data) {
  const pos   = (data.position || "NONE").toUpperCase();
  const dists = data.distances || {};

  elDistLeft.textContent   = fmtDist(dists.left);
  elDistCenter.textContent = fmtDist(dists.center);
  elDistRight.textContent  = fmtDist(dists.right);

  // Highlight active sensor
  [
    [elPosLeft,   "LEFT"],
    [elPosCenter, "CENTER"],
    [elPosRight,  "RIGHT"],
  ].forEach(([el, name]) => {
    el.classList.toggle("active", pos === name);
  });
}

// ── Update servo needle ───────────────────────────────────────────
function updateServo(data) {
  const pos   = (data.position || "NONE").toUpperCase();
  const angle = SERVO_ANGLES[pos] ?? 90;

  // Map 30–150° servo range onto -60°…+60° needle rotation
  const needleRotation = angle - 90;
  elServoNeedle.style.transform = `rotate(${needleRotation}deg)`;
}

// ── Update history table ──────────────────────────────────────────
function updateHistoryTable(records) {
  if (!records || records.length === 0) return;

  const rows = records.slice(0, MAX_HISTORY_ROWS).map((r) => {
    const state = (r.state || "").toUpperCase();
    const dists = r.distances || {};
    return `<tr>
      <td>${fmtTime(r.timestamp)}</td>
      <td class="state-${state}">${state}</td>
      <td>${r.position || "—"}</td>
      <td>${fmtDist(dists.left)}</td>
      <td>${fmtDist(dists.center)}</td>
      <td>${fmtDist(dists.right)}</td>
    </tr>`;
  });

  elHistoryBody.innerHTML = rows.join("");
}

// ── Connection badge helper ────────────────────────────────────────
function setBadge(status) {
  elBadge.className  = "badge";
  if (status === "online") {
    elBadge.classList.add("badge-online");
    elBadge.textContent = "● Online";
  } else if (status === "offline") {
    elBadge.classList.add("badge-offline");
    elBadge.textContent = "● Offline";
  } else {
    elBadge.classList.add("badge-connecting");
    elBadge.textContent = "Connecting…";
  }
}

// ── Poll /status ──────────────────────────────────────────────────
async function pollStatus() {
  try {
    const res  = await fetch("/status");
    if (!res.ok) throw new Error("HTTP " + res.status);
    const data = await res.json();

    setBadge("online");
    updateStatusCard(data);
    updatePositionCard(data);
    updateServo(data);
  } catch (err) {
    setBadge("offline");
    console.warn("Status poll error:", err);
  }
}

// ── Poll /history ─────────────────────────────────────────────────
async function pollHistory() {
  try {
    const res     = await fetch("/history");
    if (!res.ok) throw new Error("HTTP " + res.status);
    const records = await res.json();
    updateHistoryTable(records);
  } catch (err) {
    console.warn("History poll error:", err);
  }
}

// ── Boot ──────────────────────────────────────────────────────────
(function init() {
  pollStatus();
  pollHistory();
  setInterval(pollStatus,  STATUS_POLL_MS);
  setInterval(pollHistory, HISTORY_POLL_MS);
})();
