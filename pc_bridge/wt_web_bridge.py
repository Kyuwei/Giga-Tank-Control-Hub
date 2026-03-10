"""
wt_web_bridge.py — War Thunder Web Interface Bridge
====================================================
HTTP server that connects the React web interface to the War Thunder
local API and the host machine's keyboard input.

Endpoints
---------
GET  /api/health         — bridge + WT connection status
GET  /api/telemetry      — raw /indicators data from WT
GET  /api/map            — combined /map_obj.json + /map_info.json
GET  /api/map/image      — /map.img proxied as JPEG (CORS enabled)
POST /api/command        — forward a keystroke to WT via keyboard module

Usage
-----
    pip install flask flask-cors keyboard requests
    python wt_web_bridge.py

The server listens on http://localhost:8112 and is designed to run
alongside the game on the same Windows machine.  The React dev server
(http://localhost:5173) will automatically connect to it.

Note: The `keyboard` module may require running as administrator on
Windows to inject keystrokes globally.
"""

import os
import threading
import time

import keyboard
import requests
from flask import Flask, Response, jsonify, request, send_from_directory
from flask_cors import CORS

# ─── Configuration ─────────────────────────────────────────────────────────────

PORT       = 8112
WT_BASE    = "http://127.0.0.1:8111"
POLL_HZ    = 10          # How often the cache worker polls the WT API
TIMEOUT    = 0.5         # Seconds before a WT API request is abandoned
STATIC_DIR = os.path.join(os.path.dirname(__file__), "static")

# ─── In-memory cache ──────────────────────────────────────────────────────────

_lock             = threading.Lock()
_telemetry_cache  = None   # Latest /indicators payload (dict) or None
_map_cache        = None   # Latest {objects, info} payload (dict) or None
_wt_online        = False  # True when WT successfully answered the last poll

# ─── Cache worker ──────────────────────────────────────────────────────────────

def _cache_worker():
    """Background daemon that polls the WT API at POLL_HZ."""
    global _telemetry_cache, _map_cache, _wt_online
    interval = 1.0 / POLL_HZ

    while True:
        online = False

        # Telemetry
        try:
            r = requests.get(f"{WT_BASE}/indicators", timeout=TIMEOUT)
            if r.status_code == 200:
                with _lock:
                    _telemetry_cache = r.json()
                online = True
        except Exception:
            pass

        # Map
        try:
            obj_r  = requests.get(f"{WT_BASE}/map_obj.json",  timeout=TIMEOUT)
            info_r = requests.get(f"{WT_BASE}/map_info.json", timeout=TIMEOUT)
            if obj_r.status_code == 200 and info_r.status_code == 200:
                with _lock:
                    _map_cache = {
                        "objects": obj_r.json(),
                        "info":    info_r.json(),
                    }
                online = True
        except Exception:
            pass

        with _lock:
            _wt_online = online

        time.sleep(interval)


# ─── Flask app ─────────────────────────────────────────────────────────────────

app = Flask(__name__, static_folder=STATIC_DIR, static_url_path="")
# Allow any localhost origin (React dev on :5173, built app on :4173, etc.)
CORS(app, resources={r"/api/*": {"origins": ["http://localhost:*", "http://127.0.0.1:*"]}})


@app.route("/", defaults={"path": ""})
@app.route("/<path:path>")
def serve_frontend(path: str):
    """Serve the built React app. Falls back to index.html for SPA routing."""
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
    """Proxies /map.img with CORS headers so the React app can display it."""
    try:
        r = requests.get(f"{WT_BASE}/map.img", timeout=1.5)
        if r.status_code == 200:
            return Response(
                r.content,
                mimetype="image/jpeg",
                headers={"Cache-Control": "no-store"},
            )
    except Exception:
        pass
    return jsonify({"error": "Map image unavailable."}), 503


@app.route("/api/command", methods=["POST"])
def command():
    """
    Receives a JSON body {"key": "G"} and injects the keystroke globally
    via the `keyboard` module, so War Thunder receives it regardless of
    which window currently has focus.
    """
    body = request.get_json(force=True, silent=True) or {}
    key  = str(body.get("key", "")).strip()

    # Basic validation: only single printable characters or named keys
    allowed_named = {
        "space", "tab", "enter", "backspace", "escape",
        "f1","f2","f3","f4","f5","f6","f7","f8","f9","f10","f11","f12",
    }
    valid = (len(key) == 1 and key.isprintable()) or key.lower() in allowed_named
    if not valid:
        return jsonify({"error": "Invalid key value."}), 400

    try:
        keyboard.press_and_release(key.lower())
        return jsonify({"sent": key})
    except Exception as exc:
        return jsonify({"error": str(exc)}), 500


# ─── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    print("=" * 60)
    print("  War Thunder Web Bridge")
    print(f"  Listening on http://localhost:{PORT}")
    print("=" * 60)
    print("  GET  /api/health      — bridge + WT status")
    print("  GET  /api/telemetry   — live vehicle telemetry")
    print("  GET  /api/map         — tactical map data")
    print("  GET  /api/map/image   — map background image")
    print("  POST /api/command     — send keystroke to game")
    print("=" * 60)
    print("  Tip: run as administrator for global keystroke injection.")
    print()

    t = threading.Thread(target=_cache_worker, daemon=True)
    t.start()

    app.run(host="localhost", port=PORT, debug=False, use_reloader=False)
