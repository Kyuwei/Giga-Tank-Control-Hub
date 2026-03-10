// ============================================================
// Control_Hub_M4 — Cortex-M4 Co-processor sketch
// Target core : M4 Co-processor  (Tools → Target Core → M4 Co-processor)
//
// Role:
//   Receives raw telemetry lines forwarded by the M7 core over the
//   bidirectional RPC stream, parses them, and sends a compact
//   structured "PARSED|…" frame back so that the M7 can update the
//   LVGL display without performing any string operations in its
//   time-critical rendering loop.
//
// Wiring / flash split:
//   Set Tools → Flash Split to "1MB M7 + 1MB M4" (or "1.5MB M7 + 0.5MB M4")
//   before uploading either sketch.  Upload the M7 sketch first, then
//   switch Tools → Target Core to "M4 Co-processor" and upload this sketch.
//
// RPC stream protocol
//   M7 → M4 (raw)   : one telemetry line, no trailing newline needed;
//                     the M7 sends it with RPC.println().
//   M4 → M7 (parsed): "PARSED|SPD:<v>|RPM:<v>|GEAR:<v>|STATUS:<0/1>|HUD:<text>"
//                     HUD is always the last field and may contain " | ".
// ============================================================

#include "RPC.h"

// ===== RECEIVE BUFFER (raw data arriving from M7) =====
static String rpcBuffer = "";
static const size_t RPC_BUF_MAX = 256;

// ===== HELPERS =====
// Extracts the value of a key:value field delimited by '|'.
static String extractField(const String& data, const char* key, int keyLen) {
    int idx = data.indexOf(key);
    if (idx < 0) return "";
    int end = data.indexOf("|", idx + keyLen);
    return data.substring(idx + keyLen, end < 0 ? (int)data.length() : end);
}

// ===== TELEMETRY PARSING =====
// Returns a structured PARSED frame understood by the M7's apply_parsed().
// Output: "PARSED|SPD:<v>|RPM:<v>|GEAR:<v>|STATUS:<0/1/-1>|HUD:<text>"
// STATUS is -1 when the incoming data does not carry a STATUS field; the M7
// will leave the bridge-status widget unchanged in that case.
static String parse_telemetry(const String& data) {
    String spd  = extractField(data, "SPD:",  4);
    String rpm  = extractField(data, "RPM:",  4);
    String gear = extractField(data, "GEAR:", 5);

    // Build HUD text from TANK, SPD and CREW fields
    String tank = extractField(data, "TANK:", 5);
    String crew = extractField(data, "CREW:", 5);
    String hud  = "";
    if (tank.length() > 0 && spd.length() > 0 && crew.length() > 0)
        hud = tank + " | " + spd + " km/h | CREW:" + crew;

    // Bridge online/offline status (-1 = field absent in source data)
    int    status    = -1;
    int    idx_stat  = data.indexOf("STATUS:");
    if (idx_stat >= 0) {
        String s = data.substring(idx_stat + 7);
        s.trim();
        status = s.startsWith("1") ? 1 : 0;
    }

    // Build result string with a single pre-allocated buffer to minimise
    // heap fragmentation from repeated String concatenations.
    String result;
    result.reserve(64 + hud.length());
    result  = "PARSED";
    result += "|SPD:";    result += spd;
    result += "|RPM:";    result += rpm;
    result += "|GEAR:";   result += gear;
    result += "|STATUS:"; result += String(status);
    result += "|HUD:";    result += hud;
    return result;
}

// ===== SETUP =====
void setup() {
    // Initialise le canal inter-coeurs OpenAMP/RPMsg cote M4.
    // Le M7 appelle egalement RPC.begin() dans son setup() ; sans cela
    // le M4 se bloque indefiniment en attendant l'acknowledgment du M7.
    RPC.begin();
}

// ===== LOOP =====
void loop() {
    // Le M7 gere desormais le parsing de la telemetrie dans son propre
    // thread Mbed RTOS (serial_task). Le M4 n'a plus de role actif.
    delay(100);
}
