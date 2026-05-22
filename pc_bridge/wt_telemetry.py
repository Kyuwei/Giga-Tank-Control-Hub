"""
wt_telemetry.py
================
Reads the local War Thunder API (http://127.0.0.1:8111) and forwards a
compact text protocol to the Arduino GIGA R1 over USB serial. V2 adds:

  - /hudmsg parsing → DAMAGE_FEED + per-module health (MOD: + EVT: lines)
  - extended map entity types (tank vs aircraft, ally vs enemy, etc.) plus
    optional rotation derived from /map_obj.json `ex`/`ey`
  - vectorised RGB565 conversion (NumPy) — ~50× faster than the pure-Python
    loop in V1, which used to stall the bridge for ~150 ms on map changes
  - more telemetry fields (driver_state, gunner_state, engine_on_fire,
    has_speed_warning, lws, ircm)

Serial framing (one line per record, '\\n' terminated):
  SPD:..|RPM:..|GEAR:..|AMMO:..|STAB:..|FUEL:..|CREW:n/total|TANK:..|
    DRV:..|GUN:..|FIRE:..|OVER:..|LWS:..|IRCM:..|STATUS:0/1
  MAPNAME:..|MAPOBJ:x,y,T[,rot];...
  MAPRAW:<base64-RGB565 LE>
  MOD:ENG:..|TRANS:..|TURR:..|GUN:..|TRKL:..|TRKR:..
  EVT:K:msg | EVT:D:msg | EVT:A:msg
"""

from __future__ import annotations

import base64
import io
import time
from math import atan2, degrees
from typing import Optional

import requests
import serial
import serial.tools.list_ports

try:
    import numpy as np
    _NUMPY_AVAILABLE = True
except ImportError:
    _NUMPY_AVAILABLE = False

try:
    from PIL import Image as _PIL_Image
    _PIL_AVAILABLE = True
except ImportError:
    _PIL_AVAILABLE = False
    print("INFO: Pillow non installe — fond de carte desactive. "
          "Installer avec : pip install Pillow numpy")

BAUD_RATE      = 115200
WT_INDICATORS  = "http://127.0.0.1:8111/indicators"
WT_MAP_OBJ     = "http://127.0.0.1:8111/map_obj.json"
WT_MAP_INFO    = "http://127.0.0.1:8111/map_info.json"
WT_MAP_IMG     = "http://127.0.0.1:8111/map.img"
WT_HUDMSG      = "http://127.0.0.1:8111/hudmsg"

MAX_MAP_ENTITIES = 20
MAP_INTERVAL     = 2   # main loop ticks (10 Hz) between map updates → 5 Hz
MOD_INTERVAL     = 10  # main loop ticks → 1 Hz module + events stream
MAP_IMG_W        = 148
MAP_IMG_H        = 65

# ─── Damage keyword → module mapping ──────────────────────────────────────────
# Keywords are searched case-insensitively in /hudmsg damage messages.
# When matched, the corresponding module's health is decremented:
#   "destroyed"/"broken"  → -35
#   "damaged"             → -15
# Strings observed in WT messages are intentionally fragmented (engine, track…)
# so language variants and abbreviations both match.
DAMAGE_PATTERNS = [
    # (keyword, module key)
    ("left track",     "TRACK_L"),
    ("track left",     "TRACK_L"),
    ("right track",    "TRACK_R"),
    ("track right",    "TRACK_R"),
    ("track",          "TRACK_L"),     # generic "track" hits left first
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


def find_arduino_port() -> str:
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if "Arduino" in p.description or "GIGA" in p.description or "STM32" in p.description:
            print(f"Arduino detected on: {p.device}")
            return p.device
    print("Arduino not detected automatically. Available ports:")
    for p in ports:
        print(f"  {p.device} - {p.description}")
    return input("Enter port manually (e.g. COM3 or /dev/ttyACM0): ")


def http_get(url: str, timeout: float = 0.5):
    try:
        r = requests.get(url, timeout=timeout)
        if r.status_code == 200:
            return r.json()
    except (requests.exceptions.RequestException, ValueError):
        pass
    return None


def get_indicators():
    return http_get(WT_INDICATORS, 0.5)


def get_map_data():
    objs = http_get(WT_MAP_OBJ, 0.5)
    info = http_get(WT_MAP_INFO, 0.5)
    if objs is None or info is None:
        return None, None
    return objs, info


def get_hudmsg():
    return http_get(WT_HUDMSG, 0.5)


def get_encoded_map_image() -> Optional[str]:
    """
    Downloads /map.img, downscales to MAP_IMG_W x MAP_IMG_H, converts to
    RGB565 little-endian, returns base64-encoded payload.
    Uses NumPy for the per-pixel conversion (~50× faster than pure Python).
    """
    if not _PIL_AVAILABLE:
        return None
    try:
        r = requests.get(WT_MAP_IMG, timeout=1.5)
        if r.status_code != 200:
            return None
        img = _PIL_Image.open(io.BytesIO(r.content)).convert("RGB")
        img = img.resize((MAP_IMG_W, MAP_IMG_H), _PIL_Image.LANCZOS)

        if _NUMPY_AVAILABLE:
            arr = np.asarray(img, dtype=np.uint8)
            r8, g8, b8 = arr[..., 0], arr[..., 1], arr[..., 2]
            v = ((r8.astype(np.uint16) >> 3) << 11) \
              | ((g8.astype(np.uint16) >> 2) <<  5) \
              | ( b8.astype(np.uint16) >> 3)
            out = np.empty(v.size * 2, dtype=np.uint8)
            out[0::2] = (v & 0xFF).flatten()
            out[1::2] = ((v >> 8) & 0xFF).flatten()
            raw = out.tobytes()
        else:
            # Slow fallback if numpy is missing
            raw = bytearray(MAP_IMG_W * MAP_IMG_H * 2)
            idx = 0
            for r8, g8, b8 in img.getdata():
                v = ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3)
                raw[idx]     = v & 0xFF
                raw[idx + 1] = (v >> 8) & 0xFF
                idx += 2
            raw = bytes(raw)

        return base64.b64encode(raw).decode("ascii")
    except Exception:
        return None


def extract_tank_data(d):
    """Pulls the fields the Arduino renders. All keys are confirmed for
    `army: tank` in the public WT API documentation."""
    speed_kmh  = abs(d.get("speed", 0))
    rpm        = d.get("rpm", 0)
    gear       = d.get("gear", "?")
    ammo       = int(d.get("first_stage_ammo", 0))
    stab       = int(d.get("stabilizer", 0))
    crew       = int(d.get("crew_current", 0))
    crew_total = int(d.get("crew_total", 0))
    fuel       = int(max(0, d.get("fuel", -1)))
    driver     = int(d.get("driver_state", 0) or 0)
    gunner     = int(d.get("gunner_state", 0) or 0)
    fire       = 1 if d.get("engine_on_fire", False) else 0
    overspeed  = int(d.get("has_speed_warning", 0) or 0)
    lws        = int(d.get("lws", -1) or -1)
    ircm       = int(d.get("ircm", -1) or -1)

    raw_type  = d.get("type", "UNKNOWN")
    tank_name = raw_type.split("/")[-1].upper() if "/" in raw_type else str(raw_type).upper()
    tank_name = tank_name.replace("|", "_").replace("\n", "_").replace("\r", "_")

    return {
        "spd": int(speed_kmh), "rpm": int(rpm), "gear": gear, "ammo": ammo,
        "stab": stab, "crew": crew, "crew_total": crew_total, "tank": tank_name,
        "fuel": fuel, "driver": driver, "gunner": gunner, "fire": fire,
        "overspeed": overspeed, "lws": lws, "ircm": ircm,
    }


def color_dominant(color_str: str) -> str:
    """Returns 'red', 'green', 'yellow' or 'other' from a CSS-style hex string."""
    hex_str = (color_str or "").lstrip("#").lower()
    if len(hex_str) < 6:
        return "other"
    try:
        r = int(hex_str[0:2], 16)
        g = int(hex_str[2:4], 16)
        b = int(hex_str[4:6], 16)
    except ValueError:
        return "other"
    if r > 150 and r > g * 1.5 and r > b * 1.5:
        return "red"
    if g > 100 and g > r * 1.2:
        return "green"
    if r > 180 and g > 180 and b < 80:
        return "yellow"
    return "other"


def classify_entity(obj) -> str:
    """
    Maps a WT map object to a single character used by the Arduino renderer.

      T = tank ally        t = tank enemy
      P = aircraft ally    p = aircraft enemy
      O = objective / capture zone
      F = airfield
      B = bomb point
      R = respawn base
      N = unknown
    """
    type_str = (obj.get("type", "") or "").lower()
    color_kind = color_dominant(obj.get("color", ""))
    is_enemy = color_kind == "red" or bool(obj.get("blink", 0))

    if "airfield" in type_str:
        return "F"
    if "bomb_point" in type_str:
        return "B"
    if "respawn_base" in type_str:
        return "R"
    if "capture_zone" in type_str:
        return "O"
    if "aircraft" in type_str or "plane" in type_str:
        return "p" if is_enemy else "P"
    if "tank" in type_str or "ground" in type_str:
        return "t" if is_enemy else "T"
    # Fallback by colour alone
    if color_kind == "red":
        return "t"
    if color_kind == "green":
        return "T"
    if color_kind == "yellow":
        return "O"
    return "N"


def entity_rotation(obj) -> int:
    """Returns 0-359° from `ex`/`ey` if both are non-zero, else 0."""
    ex = float(obj.get("ex", 0.0) or 0.0)
    ey = float(obj.get("ey", 0.0) or 0.0)
    x  = float(obj.get("x",  0.0) or 0.0)
    y  = float(obj.get("y",  0.0) or 0.0)
    if abs(ex) < 1e-6 and abs(ey) < 1e-6:
        return 0
    deg = degrees(atan2(ey - y, ex - x))
    deg_i = int(deg) % 360
    return deg_i


def format_telem_message(td: dict, online: bool) -> str:
    return (f"SPD:{td['spd']}|RPM:{td['rpm']}|GEAR:{td['gear']}"
            f"|AMMO:{td['ammo']}|STAB:{td['stab']}|FUEL:{td['fuel']}"
            f"|CREW:{td['crew']}/{td['crew_total']}|TANK:{td['tank']}"
            f"|DRV:{td['driver']}|GUN:{td['gunner']}|FIRE:{td['fire']}"
            f"|OVER:{td['overspeed']}|LWS:{td['lws']}|IRCM:{td['ircm']}"
            f"|STATUS:{1 if online else 0}\n")


def format_map_message(objects, info) -> str:
    gen      = info.get("map_generation", 0)
    map_name = str(info.get("name", f"MAP{gen}")).upper()[:15]

    # Priority order: enemy combatants first, then objectives, allies, statics.
    PRIORITY = {"t": 0, "p": 1, "B": 2, "O": 3, "R": 4,
                "T": 5, "P": 6, "F": 7, "N": 8}

    typed = []
    for obj in objects:
        t = classify_entity(obj)
        x = float(obj.get("x", 0.0))
        y = float(obj.get("y", 0.0))
        rot = entity_rotation(obj)
        typed.append((PRIORITY.get(t, 9), x, y, t, rot))

    typed.sort(key=lambda o: o[0])
    typed = typed[:MAX_MAP_ENTITIES]

    if not typed:
        return f"MAPNAME:{map_name}|MAPOBJ:-\n"

    obj_parts = [f"{x:.2f},{y:.2f},{t},{rot}" for _, x, y, t, rot in typed]
    return f"MAPNAME:{map_name}|MAPOBJ:{';'.join(obj_parts)}\n"


def format_modules_message(mod_health: dict) -> str:
    return (f"MOD:ENG:{mod_health['ENGINE']}|TRANS:{mod_health['TRANSMISSION']}"
            f"|TURR:{mod_health['TURRET']}|GUN:{mod_health['BARREL']}"
            f"|TRKL:{mod_health['TRACK_L']}|TRKR:{mod_health['TRACK_R']}\n")


def reset_module_health() -> dict:
    return {"ENGINE": 100, "TRANSMISSION": 100, "TURRET": 100,
            "BARREL": 100, "TRACK_L": 100, "TRACK_R": 100}


def apply_damage_message(msg: str, mod_health: dict) -> bool:
    """Mutates mod_health if msg matches a known pattern. Returns True if changed."""
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


def open_serial(port: str) -> serial.Serial:
    ser = serial.Serial(port, BAUD_RATE, timeout=1)
    time.sleep(2)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    return ser


def main():
    port = find_arduino_port()
    print(f"Connecting to Arduino on {port}...")
    try:
        ser = open_serial(port)
        print("Connection established. War Thunder telemetry bridge started!")
    except serial.SerialException as e:
        print(f"Error: {e}")
        return

    offline_counter = 0
    drain_counter   = 0
    map_counter     = 0
    mod_counter     = 0
    last_map_gen    = -1
    mod_health      = reset_module_health()
    last_event_id   = 0   # bumps on each new event so we never re-emit the same line

    while True:
        data = get_indicators()

        # ── Telemetry message ────────────────────────────────────────────────
        msg = None
        if data and data.get("army") == "tank":
            offline_counter = 0
            td = extract_tank_data(data)
            msg = format_telem_message(td, online=True)
        else:
            offline_counter += 1
            if offline_counter > 30:
                msg = ("SPD:0|RPM:0|GEAR:-|AMMO:-1|STAB:0|FUEL:-1|CREW:0/0"
                       "|TANK:OFFLINE|DRV:0|GUN:0|FIRE:0|OVER:0|LWS:-1|IRCM:-1"
                       "|STATUS:0\n")
                offline_counter = 0

        if msg is not None:
            try:
                ser.write(msg.encode("utf-8"))
                ser.flush()
                print(f"> {msg.strip()}")
            except (serial.SerialException, OSError) as e:
                print(f"Serial write error: {e}. Attempting to reconnect...")
                ser.close()
                time.sleep(2)
                try:
                    ser = open_serial(port)
                    print("Reconnected successfully.")
                except serial.SerialException as reconnect_err:
                    print(f"Reconnection failed: {reconnect_err}")
                    return

        # ── Map message (2 Hz) ────────────────────────────────────────────────
        map_counter += 1
        if map_counter >= MAP_INTERVAL:
            map_counter = 0
            map_objs, map_info = get_map_data()
            if map_objs is not None and map_info is not None:
                map_msg = format_map_message(map_objs, map_info)
                try:
                    ser.write(map_msg.encode("utf-8"))
                    ser.flush()
                except (serial.SerialException, OSError):
                    pass

                # Map image background — once per new map_generation
                current_gen = int(map_info.get("map_generation", -1))
                if current_gen != last_map_gen:
                    last_map_gen = current_gen
                    mod_health   = reset_module_health()  # reset health on new match
                    encoded = get_encoded_map_image()
                    if encoded is not None:
                        try:
                            ser.write(f"MAPRAW:{encoded}\n".encode("utf-8"))
                            ser.flush()
                            print(f"> MAPRAW: {len(encoded)} chars "
                                  f"(gen={current_gen}, ~{len(encoded)//1000} KB)")
                        except (serial.SerialException, OSError):
                            pass

        # ── HUD messages & module health (1 Hz) ─────────────────────────────
        mod_counter += 1
        if mod_counter >= MOD_INTERVAL:
            mod_counter = 0
            hud = get_hudmsg()
            if hud is not None:
                for ev in hud.get("damage", []) or []:
                    ev_id = int(ev.get("id", 0))
                    if ev_id and ev_id <= last_event_id:
                        continue
                    if ev_id:
                        last_event_id = max(last_event_id, ev_id)
                    msg_text = str(ev.get("msg", "")).strip()
                    if not msg_text:
                        continue
                    apply_damage_message(msg_text, mod_health)
                    try:
                        ser.write(f"EVT:D:{msg_text[:55]}\n".encode("utf-8"))
                    except (serial.SerialException, OSError):
                        pass
                for ev in hud.get("events", []) or []:
                    ev_id = int(ev.get("id", 0))
                    if ev_id and ev_id <= last_event_id:
                        continue
                    if ev_id:
                        last_event_id = max(last_event_id, ev_id)
                    msg_text = str(ev.get("msg", "")).strip()
                    if not msg_text:
                        continue
                    kind = "K" if "destroy" in msg_text.lower() or "killed" in msg_text.lower() else "A"
                    try:
                        ser.write(f"EVT:{kind}:{msg_text[:55]}\n".encode("utf-8"))
                    except (serial.SerialException, OSError):
                        pass
                try:
                    ser.write(format_modules_message(mod_health).encode("utf-8"))
                    ser.flush()
                except (serial.SerialException, OSError):
                    pass

        # ── Periodic input-buffer drain ───────────────────────────────────────
        drain_counter += 1
        if drain_counter >= 50:
            ser.reset_input_buffer()
            drain_counter = 0

        time.sleep(0.1)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nTelemetry bridge stopped.")
