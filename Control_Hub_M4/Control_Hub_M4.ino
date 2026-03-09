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
//   En plus du parsing, le M4 mesure sa propre charge CPU et envoie
//   periodiquement un message "CPU4:{pct}\n" au M7 (une fois par seconde).
//   La charge est calculee comme le pourcentage de temps passe a traiter
//   des donnees RPC par rapport a la fenetre d'une seconde.
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
//   M4 → M7 (cpu)   : "CPU4:<pct>\n"  — charge CPU M4 en pourcentage (0-100),
//                     envoyee une fois par seconde.
// ============================================================

#include "RPC.h"

// ===== RECEIVE BUFFER (raw data arriving from M7) =====
static String rpcBuffer = "";
static const size_t RPC_BUF_MAX = 256;

// ===== MESURE CHARGE CPU M4 =====
static uint32_t m4_busy_us  = 0;   // microsecondes passees a traiter des donnees RPC
static uint32_t m4_t_window = 0;   // millis au debut de la fenetre de mesure courante

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
    RPC.begin();
    rpcBuffer.reserve(RPC_BUF_MAX);
    m4_t_window = millis();
}

// ===== LOOP =====
void loop() {
    uint32_t now_ms = millis();

    // ── Lecture et traitement des donnees RPC (M7 → M4) ──
    // Le temps passe ici est mesure pour le calcul de charge CPU.
    if (RPC.available()) {
        uint32_t t0 = micros();
        do {
            char c = (char)RPC.read();
            if (c == '\n') {
                rpcBuffer.trim();
                if (rpcBuffer.length() > 0) {
                    String result = parse_telemetry(rpcBuffer);
                    RPC.println(result);
                }
                rpcBuffer = "";
            } else {
                if (rpcBuffer.length() < RPC_BUF_MAX) {
                    rpcBuffer += c;
                } else {
                    rpcBuffer = "";  // Discard runaway frame
                }
            }
        } while (RPC.available());
        m4_busy_us += micros() - t0;
    }

    // ── Envoi periodique de la charge CPU M4 au M7 (une fois par seconde) ──
    if (now_ms - m4_t_window >= 1000) {
        uint32_t elapsed_ms = now_ms - m4_t_window;
        // load% = busy_us / (elapsed_ms * 1000) * 100 = busy_us / (elapsed_ms * 10)
        uint32_t load_pct = m4_busy_us / (elapsed_ms * 10);
        if (load_pct > 100) load_pct = 100;
        m4_busy_us  = 0;
        m4_t_window = now_ms;
        RPC.print("CPU4:");
        RPC.print((int)load_pct);
        RPC.print('\n');
    }
}
