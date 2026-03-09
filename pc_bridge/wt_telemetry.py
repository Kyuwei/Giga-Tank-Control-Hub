import requests
import serial
import serial.tools.list_ports
import time
import io
import base64

try:
    from PIL import Image as _PIL_Image
    _PIL_AVAILABLE = True
except ImportError:
    _PIL_AVAILABLE = False
    print("INFO: Pillow non installe — fond de carte desactive. "
          "Installer avec : pip install Pillow")

BAUD_RATE      = 115200
WT_INDICATORS  = "http://127.0.0.1:8111/indicators"
WT_MAP_OBJ     = "http://127.0.0.1:8111/map_obj.json"
WT_MAP_INFO    = "http://127.0.0.1:8111/map_info.json"
WT_MAP_IMG     = "http://127.0.0.1:8111/map.img"

# Maximum number of map entities sent per message (mirrors MAP_MAX_ENT in the .ino).
MAX_MAP_ENTITIES = 20
# Send a map update every MAP_INTERVAL main-loop iterations (10 Hz ÷ 2 = 5 Hz).
MAP_INTERVAL = 2
# Dimensions de l'image envoyee a l'Arduino (doit correspondre a MAP_RAW_W/H dans le .ino).
# 148 × 5 = 740 = MAP_CONT_W, 65 × 5 = 325 = MAP_CONT_H → echelle exacte 5×.
MAP_IMG_W = 148
MAP_IMG_H = 65

def find_arduino_port():
    """Detects the Arduino GIGA R1 serial port automatically."""
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if "Arduino" in p.description or "GIGA" in p.description or "STM32" in p.description:
            print(f"Arduino detected on: {p.device}")
            return p.device
    print("Arduino not detected automatically. Available ports:")
    for p in ports:
        print(f"  {p.device} - {p.description}")
    return input("Enter port manually (e.g. COM3 or /dev/ttyACM0): ")

def get_indicators():
    """Fetches telemetry data from the War Thunder localhost API."""
    try:
        r = requests.get(WT_INDICATORS, timeout=0.5)
        if r.status_code == 200:
            return r.json()
    except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
        pass
    return None

def get_map_data():
    """Fetches map objects and metadata from the War Thunder localhost API."""
    try:
        obj_r  = requests.get(WT_MAP_OBJ,  timeout=0.5)
        info_r = requests.get(WT_MAP_INFO, timeout=0.5)
        if obj_r.status_code == 200 and info_r.status_code == 200:
            return obj_r.json(), info_r.json()
    except (requests.exceptions.ConnectionError, requests.exceptions.Timeout):
        pass
    return None, None

def get_encoded_map_image():
    """
    Recupere l'image de la carte depuis War Thunder (/map.img), la redimensionne a
    (MAP_IMG_W × MAP_IMG_H) et retourne les pixels RGB565 little-endian en base64.
    Format du message envoye a l'Arduino : MAPRAW:{base64}\\n
    Retourne None si Pillow n'est pas installe ou en cas d'erreur reseau/image.
    """
    if not _PIL_AVAILABLE:
        return None
    try:
        r = requests.get(WT_MAP_IMG, timeout=1.5)
        if r.status_code != 200:
            return None
        img = _PIL_Image.open(io.BytesIO(r.content)).convert("RGB")
        img = img.resize((MAP_IMG_W, MAP_IMG_H), _PIL_Image.LANCZOS)
        # Conversion en RGB565 little-endian (format natif LVGL sur ARM Cortex-M little-endian).
        # Chaque pixel : R5 G6 B5, stocke en little-endian (octet bas en premier).
        raw = bytearray(MAP_IMG_W * MAP_IMG_H * 2)
        idx = 0
        for r8, g8, b8 in img.getdata():
            v = ((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3)
            raw[idx]     = v & 0xFF          # octet bas : GGGBBBBB
            raw[idx + 1] = (v >> 8) & 0xFF  # octet haut : RRRRRGGG
            idx += 2
        return base64.b64encode(bytes(raw)).decode('ascii')
    except Exception:
        return None

def extract_tank_data(d):
    """
    Extracts relevant data for ground vehicles.
    Keys confirmed for tanks via /indicators endpoint.
    Note: 'speed' is already in km/h in the War Thunder API response.
    """
    speed_kmh  = abs(d.get("speed", 0))
    rpm        = d.get("rpm", 0)
    gear       = d.get("gear", "?")
    ammo       = int(d.get("first_stage_ammo", 0))
    stab       = int(d.get("stabilizer", 0))
    crew       = int(d.get("crew_current", 0))
    crew_total = int(d.get("crew_total", 0))

    # Extract short tank name from model path (e.g. "tankModels/us_m1a2_abrams" -> "US_M1A2_ABRAMS")
    raw_type   = d.get("type", "UNKNOWN")
    tank_name  = raw_type.split("/")[-1].upper() if "/" in raw_type else raw_type.upper()

    return int(speed_kmh), int(rpm), gear, ammo, stab, crew, crew_total, tank_name

def color_to_type(color_str, obj_type_str):
    """
    Maps a War Thunder map object to a single-character entity type code:
      A = allié (vert)
      E = ennemi (rouge)
      O = objectif / zone de capture
      F = aerodrome
      N = autre / inconnu
    """
    obj_type_str = (obj_type_str or "").lower()

    # Structural types identified by their 'type' field
    if "airfield" in obj_type_str:
        return "F"
    if "capture_zone" in obj_type_str or "bomb_point" in obj_type_str \
            or "respawn_base" in obj_type_str:
        return "O"

    # Vehicle/unit types identified by dominant color channel
    color = (color_str or "").lstrip("#").lower()
    if len(color) < 6:
        return "N"
    try:
        r = int(color[0:2], 16)
        g = int(color[2:4], 16)
        b = int(color[4:6], 16)
    except ValueError:
        return "N"

    if r > 150 and r > g * 1.5 and r > b * 1.5:   # Rouge dominant → ennemi
        return "E"
    if g > 100 and g > r * 1.2:                    # Vert dominant  → allié
        return "A"
    if r > 180 and g > 180 and b < 80:             # Jaune          → objectif
        return "O"
    return "N"

def format_map_message(objects, info):
    """
    Builds the serial map message for the Arduino.
    Format: MAPNAME:{name}|MAPOBJ:{x},{y},{type};...\\n
    Positions are normalized floats in [0.0, 1.0] with 2 decimal places.
    Entities are prioritised: enemies first, then objectives, allies, airfields.
    """
    # Use map_generation as a compact map identifier when no name is available.
    gen      = info.get("map_generation", 0)
    map_name = info.get("name", f"MAP{gen}").upper()[:15]   # cap at 15 chars

    # Priority order: E=0, O=1, A=2, F=3, N=4
    PRIORITY = {"E": 0, "O": 1, "A": 2, "F": 3, "N": 4}

    typed = []
    for obj in objects:
        t = color_to_type(obj.get("color", ""), obj.get("type", ""))
        x = float(obj.get("x", 0.0))
        y = float(obj.get("y", 0.0))
        typed.append((PRIORITY.get(t, 4), x, y, t))

    typed.sort(key=lambda o: o[0])
    typed = typed[:MAX_MAP_ENTITIES]

    if not typed:
        return f"MAPNAME:{map_name}|MAPOBJ:-\n"

    obj_parts = [f"{x:.2f},{y:.2f},{t}" for _, x, y, t in typed]
    return f"MAPNAME:{map_name}|MAPOBJ:{';'.join(obj_parts)}\n"

def open_serial(port):
    """Opens the serial port and waits for the Arduino to reset."""
    ser = serial.Serial(port, BAUD_RATE, timeout=1)
    time.sleep(2)  # Wait for Arduino reset
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
    last_map_gen    = -1   # Detecte le changement de carte pour envoyer le fond d'image

    while True:
        data = get_indicators()

        # ── Telemetry message ────────────────────────────────────────────────
        msg = None
        if data and data.get("army") == "tank":
            offline_counter = 0
            spd, rpm, gear, ammo, stab, crew, crew_total, tank = extract_tank_data(data)

            # Compact format parsed by the Arduino
            msg = (f"SPD:{spd}|RPM:{rpm}|GEAR:{gear}"
                   f"|AMMO:{ammo}|STAB:{stab}"
                   f"|CREW:{crew}/{crew_total}|TANK:{tank}|STATUS:1\n")
        else:
            offline_counter += 1
            if offline_counter > 30:
                msg = "SPD:0|RPM:0|GEAR:-|AMMO:-|STAB:0|CREW:-/-|TANK:OFFLINE|STATUS:0\n"
                offline_counter = 0

        if msg is not None:
            try:
                ser.write(msg.encode("utf-8"))
                ser.flush()  # Ensure data is transmitted immediately
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
                    pass  # Reconnect will be handled on the next telemetry write

                # ── Fond de carte (une seule fois par partie/map_generation) ─────────
                current_gen = int(map_info.get("map_generation", -1))
                if current_gen != last_map_gen:
                    last_map_gen = current_gen
                    encoded = get_encoded_map_image()
                    if encoded is not None:
                        img_msg = f"MAPRAW:{encoded}\n"
                        try:
                            ser.write(img_msg.encode("utf-8"))
                            ser.flush()
                            print(f"> MAPRAW: {len(encoded)} chars "
                                  f"(gen={current_gen}, ~{len(encoded)//1000} KB)")
                        except (serial.SerialException, OSError):
                            pass

        # ── Periodic input-buffer drain ───────────────────────────────────────
        # Prevents USB CDC flow-control back-pressure from blocking outbound transfers.
        drain_counter += 1
        if drain_counter >= 50:  # every ~5 seconds at 10 Hz
            ser.reset_input_buffer()
            drain_counter = 0

        time.sleep(0.1)  # 10 Hz refresh rate

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nTelemetry bridge stopped.")
