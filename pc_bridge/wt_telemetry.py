import requests
import serial
import serial.tools.list_ports
import time

BAUD_RATE = 115200
WT_INDICATORS = "http://127.0.0.1:8111/indicators"

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
    drain_counter = 0

    while True:
        data = get_indicators()

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
            else:
                time.sleep(0.1)
                continue

        try:
            ser.write(msg.encode('utf-8'))
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

        # Periodically drain the input buffer to prevent USB CDC flow-control
        # back-pressure from blocking outbound transfers.
        drain_counter += 1
        if drain_counter >= 50:  # every ~5 seconds at 10 Hz
            ser.reset_input_buffer()
            drain_counter = 0

        time.sleep(0.1)  # 10 Hz refresh rate

if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\nTelemetry bridge stopped.")
