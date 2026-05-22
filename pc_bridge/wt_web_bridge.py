"""
wt_web_bridge.py — War Thunder Web Interface Bridge (V2, read-only)
=====================================================================
HTTP server that connects the React web interface to the War Thunder
local API. V2 is fully passive: it no longer injects keystrokes; the
hub displays telemetry only.

Endpoints
---------
GET  /api/health        — bridge + WT connection status
GET  /api/telemetry     — raw /indicators data from WT
GET  /api/map           — combined /map_obj.json + /map_info.json
GET  /api/map/image     — /map.img proxied as JPEG (CORS enabled)
GET  /api/events        — last N HUD events (kills, damage, alerts)
GET  /api/modules       — synthesised module health (0-100 per module)

Usage
-----
    pip install flask flask-cors requests
    python wt_web_bridge.py
"""

import os
import threading
import time
from collections import deque

import requests
from flask import Flask, Response, jsonify, send_from_directory
from flask_cors import CORS

PORT       = 8112
WT_BASE    = "http://127.0.0.1:8111"
POLL_HZ    = 10
HUD_HZ     = 2
TIMEOUT    = 0.5
STATIC_DIR = os.path.join(os.path.dirname(__file__), "static")
EVENTS_MAX = 50

# ─── Damage patterns (mirrors wt_telemetry.py) ────────────────────────────────
DAMAGE_PATTERNS = [
    ("left track",     "TRACK_L"),
    ("track left",     "TRACK_L"),
    ("right track",    "TRACK_R"),
    ("track right",    "TRACK_R"),
    ("track",          "TRACK_L"),
    ("transmission",   "TRANSMISSION"),
    ("gearbox",        "TRANSMISSION"),
    ("engine",         "ENGINE"),
    ("barrel",         "BARREL"),
    ("cannon",         "BARREL"),
    ("turret rotation","TURRET"),
    ("turret drive",   "TURRET"),
    ("turret",         "TURRET"),
]
DESTROY_WORDS = ("destroyed", "broken", "knocked out", "shattered")
DAMAGE_WORDS  = ("damaged", "jammed", "hit", "critical")


def reset_module_health() -> dict:
    return {"ENGINE": 100, "TRANSMISSION": 100, "TURRET": 100,
            "BARREL": 100, "TRACK_L": 100, "TRACK_R": 100}


# ─── In-memory cache ──────────────────────────────────────────────────────────

_lock             = threading.Lock()
_telemetry_cache  = None
_map_cache        = None
_wt_online        = False
_events           = deque(maxlen=EVENTS_MAX)
_module_health    = reset_module_health()
_last_event_id    = 0
_last_map_gen     = -1


def _http_get(url: str, timeout: float = TIMEOUT):
    try:
        r = requests.get(url, timeout=timeout)
        if r.status_code == 200:
            return r.json()
    except (requests.exceptions.RequestException, ValueError):
        pass
    return None


def _apply_damage(msg: str, mod_health: dict) -> bool:
    text = (msg or "").lower()
    decrement = 0
    if any(w in text for w in DESTROY_WORDS):
        decrement = 35
    elif any(w in text for w in DAMAGE_WORDS):
        decrement = 15
    if decrement == 0:
        return False
    for keyword, module in DAMAGE_PATTERNS:
        if keyword in text:
            mod_health[module] = max(0, mod_health[module] - decrement)
            return True
    return False


# ─── Cache worker ─────────────────────────────────────────────────────────────

def _cache_worker():
    global _telemetry_cache, _map_cache, _wt_online, _last_map_gen
    interval = 1.0 / POLL_HZ
    hud_every = max(1, POLL_HZ // HUD_HZ)
    tick = 0

    while True:
        online = False

        tel = _http_get(f"{WT_BASE}/indicators")
        if tel is not None:
            with _lock:
                _telemetry_cache = tel
            online = True

        obj  = _http_get(f"{WT_BASE}/map_obj.json")
        info = _http_get(f"{WT_BASE}/map_info.json")
        if obj is not None and info is not None:
            with _lock:
                _map_cache = {"objects": obj, "info": info}
                gen = int(info.get("map_generation", -1))
                if gen != _last_map_gen:
                    _last_map_gen = gen
                    _module_health.update(reset_module_health())
                    _events.clear()
            online = True

        tick += 1
        if tick % hud_every == 0:
            hud = _http_get(f"{WT_BASE}/hudmsg")
            if hud is not None:
                global _last_event_id
                with _lock:
                    for ev in (hud.get("damage", []) or []):
                        ev_id = int(ev.get("id", 0) or 0)
                        if ev_id and ev_id <= _last_event_id:
                            continue
                        if ev_id:
                            _last_event_id = max(_last_event_id, ev_id)
                        msg_text = str(ev.get("msg", "")).strip()
                        if not msg_text:
                            continue
                        _apply_damage(msg_text, _module_health)
                        _events.append({
                            "kind": "damage",
                            "msg":  msg_text[:96],
                            "ts":   int(time.time()),
                            "enemy": bool(ev.get("enemy", False)),
                        })
                    for ev in (hud.get("events", []) or []):
                        ev_id = int(ev.get("id", 0) or 0)
                        if ev_id and ev_id <= _last_event_id:
                            continue
                        if ev_id:
                            _last_event_id = max(_last_event_id, ev_id)
                        msg_text = str(ev.get("msg", "")).strip()
                        if not msg_text:
                            continue
                        lower = msg_text.lower()
                        kind = "kill" if ("destroy" in lower or "killed" in lower) else "alert"
                        _events.append({
                            "kind": kind,
                            "msg":  msg_text[:96],
                            "ts":   int(time.time()),
                            "enemy": bool(ev.get("enemy", False)),
                        })

        with _lock:
            _wt_online = online

        time.sleep(interval)


# ─── Flask app ────────────────────────────────────────────────────────────────

app = Flask(__name__, static_folder=STATIC_DIR, static_url_path="")
CORS(app, resources={r"/api/*": {"origins": ["http://localhost:*", "http://127.0.0.1:*"]}})


@app.route("/", defaults={"path": ""})
@app.route("/<path:path>")
def serve_frontend(path: str):
    if path and os.path.exists(os.path.join(STATIC_DIR, path)):
        return send_from_directory(STATIC_DIR, path)
    return send_from_directory(STATIC_DIR, "index.html")


@app.route("/api/health")
def health():
    with _lock:
        wt = _wt_online
    return jsonify({"status": "ok", "wt_online": wt})


@app.route("/api/telemetry")
def telemetry():
    with _lock:
        data = _telemetry_cache
    if data is None:
        return jsonify({"error": "War Thunder is not running or not in a match."}), 503
    return jsonify(data)


@app.route("/api/map")
def map_data():
    with _lock:
        data = _map_cache
    if data is None:
        return jsonify({"error": "War Thunder is not running or not in a match."}), 503
    return jsonify(data)


@app.route("/api/map/image")
def map_image():
    try:
        r = requests.get(f"{WT_BASE}/map.img", timeout=1.5)
        if r.status_code == 200:
            return Response(r.content, mimetype="image/jpeg",
                            headers={"Cache-Control": "no-store"})
    except Exception:
        pass
    return jsonify({"error": "Map image unavailable."}), 503


@app.route("/api/events")
def events():
    with _lock:
        return jsonify({"events": list(_events)})


@app.route("/api/modules")
def modules():
    with _lock:
        return jsonify(dict(_module_health))


# ─── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    print("=" * 60)
    print("  War Thunder Web Bridge V2 — READ-ONLY")
    print(f"  Listening on http://localhost:{PORT}")
    print("=" * 60)
    print("  GET  /api/health     — bridge + WT status")
    print("  GET  /api/telemetry  — live vehicle telemetry")
    print("  GET  /api/map        — tactical map data")
    print("  GET  /api/map/image  — map background image")
    print("  GET  /api/events     — live HUD event feed")
    print("  GET  /api/modules    — synthesised module health")
    print("=" * 60)

    t = threading.Thread(target=_cache_worker, daemon=True)
    t.start()

    app.run(host="127.0.0.1", port=PORT, debug=False, use_reloader=False)
