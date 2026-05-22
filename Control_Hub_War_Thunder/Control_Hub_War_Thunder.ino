// ============================================================
// Control_Hub_War_Thunder — Arduino GIGA R1 sketch (single-core, M7)
// Target core : Main Core  (Tools → Target Core → Main Core)
//
// V2 — read-only telemetry hub. The USB HID keyboard injection has been
// removed (DirectInput in War Thunder consumed only every other keystroke,
// making the feature unreliable). The hub now passively renders telemetry,
// damage events, modules health and a tactical map with proper vector-ish
// icons. A splash screen is shown at boot.
//
// Architecture:
//   - M7 owns LVGL, touch and the USB serial link to wt_telemetry.py.
//   - A dedicated Mbed RTOS thread (g_serial_thread) reads bytes from
//     Serial into fixed char buffers (no Arduino String → no heap
//     fragmentation), parses them, and writes POD structs protected by
//     g_data_mutex. LVGL is touched only from the main loop.
//   - The M4 core is intentionally unused: RPC inter-core latency
//     (~5-10 ms per round-trip) exceeded the gain from offloading any
//     of the current workloads (10 Hz parsing, rare base64 decode).
// ============================================================

#include "Arduino_H7_Video.h"
#include "Arduino_GigaDisplayTouch.h"
#include "lvgl.h"
#include "mbed.h"

// LVGL 9 may not expose LV_IMAGE_HEADER_MAGIC in every Arduino bundle;
// declare it before including the generated image headers.
#ifndef LV_IMAGE_HEADER_MAGIC
#  define LV_IMAGE_HEADER_MAGIC  0x19U
#endif

#include "splash_image.h"
#include "map_icons.h"

// ===== HARDWARE =====
Arduino_H7_Video Display(800, 480, GigaDisplayShield);
Arduino_GigaDisplayTouch TouchDetector;

// ===== ECRANS =====
lv_obj_t * screen_splash;
lv_obj_t * screen_overview;
lv_obj_t * screen_telem;
lv_obj_t * screen_map;

// ===== WIDGETS GLOBAUX =====
lv_obj_t * hud_label_btn;
lv_obj_t * telem_spd;
lv_obj_t * telem_rpm;
lv_obj_t * telem_gear;
lv_obj_t * telem_status;

// ===== CARTE =====
// Maximum number of map entities displayed simultaneously.
#define MAP_MAX_ENT  20
// Inner dimensions of the map container (pixels).
#define MAP_CONT_W   740
#define MAP_CONT_H   325

// ===== IMAGE D'ARRIERE-PLAN CARTE =====
// Dimensions de l'image RGB565 recue du PC (doit correspondre a MAP_IMG_W/H dans wt_telemetry.py).
// 148 × 5 = 740 = MAP_CONT_W, 65 × 5 = 325 = MAP_CONT_H → echelle 5× exacte, aucun rognage.
#define MAP_RAW_W      148
#define MAP_RAW_H      65
// Facteur d'echelle LVGL : 256 = 100%, donc 5× = 1280.
#define MAP_BG_SCALE   1280
// Taille du tampon RGB565 en octets (2 octets par pixel).
#define MAP_RAW_BYTES  (MAP_RAW_W * MAP_RAW_H * 2)
// Taille max du payload base64 (ceil(MAP_RAW_BYTES × 4 / 3) + marge de securite).
#define MAP_B64_MAX    26000

// ===== EVENT FEED =====
// Ring buffer of the last EVENT_MAX events (kills, damage, alerts).
#define EVENT_MAX      8
#define EVENT_MSG_LEN  56

static lv_obj_t * map_name_label;
static lv_obj_t * map_wait_label;
static lv_obj_t * map_container;
static lv_obj_t * map_icons_obj[MAP_MAX_ENT];

// Tampon RGB565 pour l'image d'arriere-plan (écrit sous mutex, lu par le thread M7).
static uint8_t        g_map_raw[MAP_RAW_BYTES];
// Descripteur d'image LVGL pointant en permanence sur g_map_raw.
static lv_image_dsc_t map_img_dsc;
// Widget image d'arriere-plan (cree et utilise dans le thread M7 uniquement).
static lv_obj_t     * map_bg_img    = NULL;
// Drapeau : g_map_raw contient de nouveaux pixels a afficher (protege par g_data_mutex).
static volatile bool  g_img_updated = false;

// ===== DONNEES PARTAGEES (thread serial <-> thread LVGL/M7) =====
struct TelemShared {
    int  spd;
    int  rpm;
    char gear[8];
    int  ammo;       // munitions 1re soute (-1 = non disponible)
    int  stab;       // stabilisateur actif
    int  crew;
    int  crew_total;
    char tank[48];
    bool online;
    int  fuel;       // carburant en % (-1 = non disponible)
    int  driver;     // 0 = vivant, !=0 = blesse/mort
    int  gunner;     // 0 = vivant, !=0 = blesse/mort
    int  engine_fire;
    int  overspeed;
    int  lws;
    int  ircm;
};

struct MapEntShared {
    float x;
    float y;
    char  type;      // T=tank_ally t=tank_enemy P=aircraft_ally p=aircraft_enemy O F B R N
    int16_t rot_deg; // 0-359 (or 0 if not provided)
};

struct MapShared {
    char         name[16];
    MapEntShared ents[MAP_MAX_ENT];
    int          count;
};

struct ModulesShared {
    uint8_t engine;
    uint8_t transmission;
    uint8_t turret;
    uint8_t barrel;
    uint8_t track_l;
    uint8_t track_r;
    bool    valid;     // false until the first MOD: line has been received
};

struct EventShared {
    char     kind;                    // 'K' = kill, 'D' = damage, 'A' = alert
    char     msg[EVENT_MSG_LEN];
    uint32_t age_ms;                  // millis() when the event was received
    bool     valid;
};

static TelemShared    g_telem;
static MapShared      g_map;
static ModulesShared  g_modules;
static EventShared    g_events[EVENT_MAX];
static int            g_events_head = 0;   // next slot to write (mod EVENT_MAX)
static rtos::Mutex    g_data_mutex;        // guards g_telem + g_map + g_modules + g_events
static volatile bool  g_telem_updated   = false;
static volatile bool  g_map_updated     = false;
static volatile bool  g_modules_updated = false;
static volatile bool  g_events_updated  = false;

// Serial-reader thread (Mbed RTOS).
static rtos::Thread g_serial_thread(osPriorityHigh, 8192);

// ===== WIDGETS OVERVIEW (screen 1) =====
static lv_obj_t * ov_speed_val      = NULL;
static lv_obj_t * ov_rpm_val        = NULL;
static lv_obj_t * ov_rpm_bar        = NULL;
static lv_obj_t * ov_gear_val       = NULL;
static lv_obj_t * ov_fuel_val       = NULL;
static lv_obj_t * ov_fuel_bar       = NULL;
static lv_obj_t * ov_crew_val       = NULL;
static lv_obj_t * ov_crew_bar       = NULL;
static lv_obj_t * ov_driver_val     = NULL;
static lv_obj_t * ov_gunner_val     = NULL;
static lv_obj_t * ov_stab_val       = NULL;
static lv_obj_t * ov_warn_fire      = NULL;
static lv_obj_t * ov_warn_over      = NULL;
static lv_obj_t * ov_warn_lws       = NULL;
static lv_obj_t * ov_warn_ircm      = NULL;
static lv_obj_t * ov_warn_ammo      = NULL;
static lv_obj_t * ov_warn_fuel      = NULL;
static lv_obj_t * ov_event_labels[EVENT_MAX] = { NULL };
static lv_obj_t * hud_bridge_lbl    = NULL;
static lv_obj_t * hud_time_lbl      = NULL;

// ===== WIDGETS VEHICLE STATUS (screen 2) =====
static lv_obj_t * telem_crew_bar    = NULL;
static lv_obj_t * telem_crew_count  = NULL;
static lv_obj_t * telem_ammo_lbl    = NULL;
static lv_obj_t * telem_ammo_bar    = NULL;
static lv_obj_t * telem_fuel_lbl    = NULL;
// Module bars (6) — labels + value bars are tracked so we can update them from g_modules.
struct ModuleWidget { lv_obj_t * bar; lv_obj_t * pct; };
static ModuleWidget mod_widgets[6];
static const char * MODULE_NAMES[6] = {
    "ENGINE", "TRANSMISSION", "TURRET DRV", "GUN BARREL", "TRACK L", "TRACK R"
};

// ===== COULEURS =====
lv_color_t COL_NEON;
lv_color_t COL_DANGER;
lv_color_t COL_WARN;
lv_color_t COL_DARK;
lv_color_t COL_BAR;
lv_color_t COL_DIM;

// ===== SWITCH D'ECRANS =====
static void cb_goto_telem(lv_event_t * e) {
    lv_screen_load_anim(screen_telem, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}
static void cb_goto_overview(lv_event_t * e) {
    lv_screen_load_anim(screen_overview, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}
static void cb_goto_map(lv_event_t * e) {
    lv_screen_load_anim(screen_map, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

// ============================================================
// HELPERS — fast in-place parsing (no Arduino String → no heap allocs)
// ============================================================

// Returns a pointer to the character following key in haystack, or NULL.
// Comparison is exact and starts only at the beginning of haystack or after a '|'.
static const char * find_field(const char * haystack, size_t len, const char * key) {
    size_t klen = strlen(key);
    if (len < klen) return NULL;
    for (size_t i = 0; i + klen <= len; i++) {
        if (i != 0 && haystack[i - 1] != '|') continue;
        if (memcmp(haystack + i, key, klen) == 0) return haystack + i + klen;
    }
    return NULL;
}

// Reads an integer field that ends at '|' or end-of-string. Returns def if absent.
static int parse_int_field(const char * s, size_t len, const char * key, int def) {
    const char * p = find_field(s, len, key);
    if (!p) return def;
    return (int)strtol(p, NULL, 10);
}

// Copies a string field (terminated by '|' or end-of-string) into out, NUL-terminated.
static void parse_str_field(const char * s, size_t len, const char * key,
                            char * out, size_t out_sz, const char * def) {
    const char * p = find_field(s, len, key);
    if (!p) { strncpy(out, def, out_sz - 1); out[out_sz - 1] = '\0'; return; }
    const char * end = strchr(p, '|');
    size_t fl = end ? (size_t)(end - p) : strlen(p);
    if (fl >= out_sz) fl = out_sz - 1;
    memcpy(out, p, fl);
    out[fl] = '\0';
    // Trim trailing \r and whitespace
    while (fl > 0 && (out[fl - 1] == '\r' || out[fl - 1] == ' ' || out[fl - 1] == '\n')) {
        out[--fl] = '\0';
    }
}

// ===== DECODAGE BASE64 (thread serie) =====
// Autonome, sans librairie externe.
// Retourne le nombre d'octets ecrits dans dst, ou 0 si src_len == 0.
static uint32_t b64_decode_buf(const uint8_t * src, size_t src_len,
                                uint8_t * dst,       size_t dst_max) {
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    uint32_t out  = 0;
    int      acc  = 0;
    int      bits = 0;
    for (size_t i = 0; i < src_len; i++) {
        int v = T[(uint8_t)src[i]];
        if (v < 0) { if (v == -2) break; continue; }
        acc   = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out >= dst_max) break;
            dst[out++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return out;
}

// ============================================================
// PANEL HELPERS (LVGL widget factories)
// ============================================================

static lv_obj_t * make_panel(lv_obj_t * parent,
                             int32_t x, int32_t y, int32_t w, int32_t h,
                             const char * title) {
    lv_obj_t * p = lv_obj_create(parent);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x0A1A0A), LV_PART_MAIN);
    lv_obj_set_style_border_color(p, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_opa(p, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_border_width(p, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(p, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p, 8, LV_PART_MAIN);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    if (title) {
        lv_obj_t * ttl = lv_label_create(p);
        lv_label_set_text(ttl, title);
        lv_obj_set_style_text_color(ttl, COL_NEON, LV_PART_MAIN);
        lv_obj_set_style_text_font(ttl, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_pos(ttl, 0, -2);

        lv_obj_t * sep = lv_obj_create(p);
        lv_obj_set_size(sep, w - 20, 1);
        lv_obj_set_pos(sep, 0, 18);
        lv_obj_set_style_bg_color(sep, COL_NEON, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sep, LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
        lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
    }
    return p;
}

static lv_obj_t * make_label(lv_obj_t * parent, const char * txt, lv_color_t col,
                             int32_t x, int32_t y) {
    lv_obj_t * l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, col, LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t * make_nav_btn(lv_obj_t * parent, const char * label,
                               int32_t x, int32_t y, int32_t w, int32_t h,
                               lv_event_cb_t cb) {
    lv_obj_t * b = lv_btn_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x001835), LV_PART_MAIN);
    lv_obj_set_style_border_color(b, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * lbl = lv_label_create(b);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(lbl);
    return b;
}

// ============================================================
// SCREEN 1 — OVERVIEW (vitals + warnings + drivetrain + event feed)
// ============================================================

static void build_screen_overview() {
    lv_obj_set_style_bg_color(screen_overview, COL_DARK, LV_PART_MAIN);

    // ── HEADER (55 px) ──────────────────────────────────────────────────────
    lv_obj_t * hdr = lv_obj_create(screen_overview);
    lv_obj_set_size(hdr, 800, 55);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x0A1A0A), LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 0, LV_PART_MAIN);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * hdr_sep = lv_obj_create(screen_overview);
    lv_obj_set_size(hdr_sep, 800, 2);
    lv_obj_set_pos(hdr_sep, 0, 55);
    lv_obj_set_style_bg_color(hdr_sep, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hdr_sep, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr_sep, 0, LV_PART_MAIN);
    lv_obj_remove_flag(hdr_sep, LV_OBJ_FLAG_SCROLLABLE);

    make_label(hdr, "SYSTEM", COL_DIM, 8, 4);
    make_label(hdr, "ONLINE", COL_NEON, 8, 25);

    make_label(hdr, "BRIDGE", COL_DIM, 115, 4);
    hud_bridge_lbl = make_label(hdr, "OFFLINE", COL_DANGER, 115, 25);

    hud_label_btn = lv_label_create(hdr);
    lv_label_set_text(hud_label_btn, "AWAITING TELEMETRY...");
    lv_obj_set_style_text_color(hud_label_btn, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(hud_label_btn, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_width(hud_label_btn, 230);
    lv_label_set_long_mode(hud_label_btn, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(hud_label_btn, 218, 14);

    make_nav_btn(hdr, LV_SYMBOL_GPS " TACTICAL MAP", 460, 9, 150, 38, cb_goto_map);
    make_nav_btn(hdr, LV_SYMBOL_RIGHT " SYS STATUS", 618, 9, 130, 38, cb_goto_telem);

    hud_time_lbl = make_label(hdr, "00:00:00", COL_NEON, 0, 0);
    lv_obj_align(hud_time_lbl, LV_ALIGN_RIGHT_MID, -6, 0);

    // ── ROW 1 (y=65, h=130) — VITALS | WARNINGS | DRIVETRAIN ────────────────
    const int32_t Y1 = 65, H1 = 130;
    const int32_t W_VITAL = 260, W_WARN = 260, W_DRIVE = 260;
    const int32_t M = 10, G = 10;

    // VITALS panel
    lv_obj_t * pv = make_panel(screen_overview, M, Y1, W_VITAL, H1, "VITALS");
    make_label(pv, "DRIVER",  COL_DIM, 0, 28);
    ov_driver_val = make_label(pv, "--", COL_DIM, 0, 28);
    lv_obj_align(ov_driver_val, LV_ALIGN_TOP_RIGHT, 0, 28);

    make_label(pv, "GUNNER",  COL_DIM, 0, 48);
    ov_gunner_val = make_label(pv, "--", COL_DIM, 0, 48);
    lv_obj_align(ov_gunner_val, LV_ALIGN_TOP_RIGHT, 0, 48);

    make_label(pv, "CREW", COL_DIM, 0, 68);
    ov_crew_val = make_label(pv, "--/--", COL_DIM, 0, 68);
    lv_obj_align(ov_crew_val, LV_ALIGN_TOP_RIGHT, 0, 68);

    ov_crew_bar = lv_bar_create(pv);
    lv_obj_set_size(ov_crew_bar, W_VITAL - 20, 6);
    lv_obj_set_pos(ov_crew_bar, 0, 88);
    lv_bar_set_range(ov_crew_bar, 0, 100);
    lv_bar_set_value(ov_crew_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ov_crew_bar, lv_color_hex(0x0A3302), LV_PART_MAIN);
    lv_obj_set_style_border_width(ov_crew_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ov_crew_bar, COL_NEON, LV_PART_INDICATOR);

    make_label(pv, "STAB", COL_DIM, 0, 100);
    ov_stab_val = make_label(pv, "--", COL_DIM, 0, 100);
    lv_obj_align(ov_stab_val, LV_ALIGN_TOP_RIGHT, 0, 100);

    // WARNINGS panel — 6 indicators in a 2×3 grid
    lv_obj_t * pw = make_panel(screen_overview, M + W_VITAL + G, Y1, W_WARN, H1, "WARNINGS");
    const int32_t WX[2] = { 0, 122 };
    const int32_t WY[3] = { 28, 60, 92 };
    ov_warn_fire  = make_label(pw, "ENGINE FIRE", COL_DIM, WX[0], WY[0]);
    ov_warn_over  = make_label(pw, "OVERSPEED",   COL_DIM, WX[1], WY[0]);
    ov_warn_lws   = make_label(pw, "LWS",         COL_DIM, WX[0], WY[1]);
    ov_warn_ircm  = make_label(pw, "IRCM",        COL_DIM, WX[1], WY[1]);
    ov_warn_ammo  = make_label(pw, "AMMO LOW",    COL_DIM, WX[0], WY[2]);
    ov_warn_fuel  = make_label(pw, "FUEL LOW",    COL_DIM, WX[1], WY[2]);

    // DRIVETRAIN panel
    lv_obj_t * pd = make_panel(screen_overview, M + W_VITAL + G + W_WARN + G, Y1, W_DRIVE, H1, "DRIVETRAIN");
    make_label(pd, "SPEED", COL_DIM, 0, 28);
    ov_speed_val = lv_label_create(pd);
    lv_label_set_text(ov_speed_val, "--");
    lv_obj_set_style_text_color(ov_speed_val, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(ov_speed_val, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ov_speed_val, LV_ALIGN_TOP_RIGHT, -34, 28);
    lv_obj_t * unit1 = make_label(pd, "KM/H", COL_DIM, 0, 0);
    lv_obj_align(unit1, LV_ALIGN_TOP_RIGHT, 0, 28);

    make_label(pd, "RPM",  COL_DIM, 0, 48);
    ov_rpm_val  = make_label(pd, "--", COL_NEON, 0, 48);
    lv_obj_align(ov_rpm_val, LV_ALIGN_TOP_RIGHT, 0, 48);
    ov_rpm_bar = lv_bar_create(pd);
    lv_obj_set_size(ov_rpm_bar, W_DRIVE - 20, 5);
    lv_obj_set_pos(ov_rpm_bar, 0, 68);
    lv_bar_set_range(ov_rpm_bar, 0, 3000);
    lv_obj_set_style_bg_color(ov_rpm_bar, lv_color_hex(0x0A3302), LV_PART_MAIN);
    lv_obj_set_style_border_width(ov_rpm_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ov_rpm_bar, COL_NEON, LV_PART_INDICATOR);

    make_label(pd, "GEAR", COL_DIM, 0, 80);
    ov_gear_val = make_label(pd, "--", COL_NEON, 0, 80);
    lv_obj_align(ov_gear_val, LV_ALIGN_TOP_RIGHT, 0, 80);

    make_label(pd, "FUEL", COL_DIM, 0, 100);
    ov_fuel_val = make_label(pd, "--", COL_NEON, 0, 100);
    lv_obj_align(ov_fuel_val, LV_ALIGN_TOP_RIGHT, 0, 100);
    ov_fuel_bar = lv_bar_create(pd);
    lv_obj_set_size(ov_fuel_bar, W_DRIVE - 20, 5);
    lv_obj_set_pos(ov_fuel_bar, 0, 116);
    lv_bar_set_range(ov_fuel_bar, 0, 100);
    lv_obj_set_style_bg_color(ov_fuel_bar, lv_color_hex(0x0A3302), LV_PART_MAIN);
    lv_obj_set_style_border_width(ov_fuel_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ov_fuel_bar, COL_NEON, LV_PART_INDICATOR);

    // ── ROW 2 (y=205, h=235) — EVENT FEED ───────────────────────────────────
    lv_obj_t * pe = make_panel(screen_overview, M, 205, 800 - 2 * M, 235, "EVENT FEED");
    for (int i = 0; i < EVENT_MAX; i++) {
        ov_event_labels[i] = lv_label_create(pe);
        lv_label_set_text(ov_event_labels[i], "");
        lv_obj_set_style_text_color(ov_event_labels[i], COL_DIM, LV_PART_MAIN);
        lv_obj_set_style_text_font(ov_event_labels[i], &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_width(ov_event_labels[i], 760);
        lv_label_set_long_mode(ov_event_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_pos(ov_event_labels[i], 0, 28 + i * 24);
    }

    // ── BOTTOM BAR ──────────────────────────────────────────────────────────
    lv_obj_t * bot = lv_obj_create(screen_overview);
    lv_obj_set_size(bot, 800, 38);
    lv_obj_align(bot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bot, lv_color_hex(0x0A1A0A), LV_PART_MAIN);
    lv_obj_set_style_border_color(bot, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_opa(bot, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(bot, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(bot, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * bl = make_label(bot, "TELEMETRY: READ-ONLY", COL_DIM, 0, 0);
    lv_obj_align(bl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t * br = make_label(bot, "ARMAMENT: HOT", COL_DIM, 0, 0);
    lv_obj_align(br, LV_ALIGN_RIGHT_MID, -8, 0);
}

// ============================================================
// SCREEN 2 — VEHICLE STATUS (detailed telemetry + dynamic modules)
// ============================================================

static void make_module_bar(lv_obj_t * parent, const char * name_str,
                            int slot_idx, int32_t by) {
    lv_obj_t * name = lv_label_create(parent);
    lv_label_set_text(name, name_str);
    lv_obj_set_style_text_color(name, COL_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(name, 0, by);

    lv_obj_t * bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, 125, by + 1);
    lv_obj_set_size(bar, 200, 12);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 100, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0A3302), LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, COL_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_NEON, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);

    lv_obj_t * pct = lv_label_create(parent);
    lv_label_set_text(pct, "--");
    lv_obj_set_style_text_color(pct, COL_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(pct, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(pct, 332, by);

    mod_widgets[slot_idx].bar = bar;
    mod_widgets[slot_idx].pct = pct;
}

static void make_drive_box(lv_obj_t * parent, const char * title_str,
                           lv_obj_t ** val_out, const char * unit_str,
                           int32_t bx, int32_t by) {
    const int32_t BW = 178, BH = 65;
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_set_pos(box, bx, by);
    lv_obj_set_size(box, BW, BH);
    lv_obj_set_style_bg_color(box, COL_DARK, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, COL_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 6, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    make_label(box, title_str, COL_DIM, 0, 0);
    *val_out = make_label(box, "--", COL_NEON, 0, 0);
    lv_obj_align(*val_out, LV_ALIGN_CENTER, 0, 4);
    lv_obj_t * unit = make_label(box, unit_str, COL_DIM, 0, 0);
    lv_obj_align(unit, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void build_screen_telem() {
    lv_obj_set_style_bg_color(screen_telem, COL_DARK, LV_PART_MAIN);

    // ── HEADER ──────────────────────────────────────────────────────────────
    lv_obj_t * hdr = lv_obj_create(screen_telem);
    lv_obj_set_size(hdr, 800, 55);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x0A1A0A), LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 0, LV_PART_MAIN);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * sep = lv_obj_create(screen_telem);
    lv_obj_set_size(sep, 800, 2);
    lv_obj_set_pos(sep, 0, 55);
    lv_obj_set_style_bg_color(sep, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE);

    make_nav_btn(hdr, LV_SYMBOL_LEFT, 8, 8, 50, 38, cb_goto_overview);

    make_label(hdr, "DIAGNOSTICS", COL_DIM, 68, 4);
    telem_status = make_label(hdr, "BRIDGE OFFLINE", COL_DANGER, 68, 25);

    lv_obj_t * t = make_label(hdr, "VEHICLE TELEMETRY", COL_DIM, 0, 0);
    lv_obj_align(t, LV_ALIGN_RIGHT_MID, -10, 0);

    // ── LEFT COLUMN (x=10, w=375) ───────────────────────────────────────────

    // CREW STATUS
    lv_obj_t * crew_panel = make_panel(screen_telem, 10, 65, 375, 173, "CREW STATUS");
    make_label(crew_panel, "PERSONNEL", COL_DIM, 0, 28);
    telem_crew_count = make_label(crew_panel, "--/--", COL_NEON, 0, 28);
    lv_obj_align(telem_crew_count, LV_ALIGN_TOP_RIGHT, 0, 28);

    telem_crew_bar = lv_bar_create(crew_panel);
    lv_obj_set_size(telem_crew_bar, 355, 14);
    lv_obj_set_pos(telem_crew_bar, 0, 52);
    lv_bar_set_range(telem_crew_bar, 0, 100);
    lv_bar_set_value(telem_crew_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(telem_crew_bar, lv_color_hex(0x0A3302), LV_PART_MAIN);
    lv_obj_set_style_border_color(telem_crew_bar, COL_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(telem_crew_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(telem_crew_bar, COL_NEON, LV_PART_INDICATOR);
    lv_obj_set_style_radius(telem_crew_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(telem_crew_bar, 2, LV_PART_INDICATOR);

    make_label(crew_panel, "STABILIZER", COL_DIM, 0, 100);
    make_label(crew_panel, "ENGINE FIRE", COL_DIM, 0, 124);

    // AMMUNITION
    lv_obj_t * ammo_panel = make_panel(screen_telem, 10, 248, 375, 168, "AMMUNITION");
    make_label(ammo_panel, "MAIN ROUND (1ST STAGE)", COL_DIM, 0, 28);
    telem_ammo_lbl = make_label(ammo_panel, "--", COL_NEON, 0, 28);
    lv_obj_align(telem_ammo_lbl, LV_ALIGN_TOP_RIGHT, 0, 28);

    telem_ammo_bar = lv_bar_create(ammo_panel);
    lv_obj_set_size(telem_ammo_bar, 355, 10);
    lv_obj_set_pos(telem_ammo_bar, 0, 52);
    lv_bar_set_range(telem_ammo_bar, 0, 40);
    lv_obj_set_style_bg_color(telem_ammo_bar, lv_color_hex(0x0A3302), LV_PART_MAIN);
    lv_obj_set_style_border_color(telem_ammo_bar, COL_DIM, LV_PART_MAIN);
    lv_obj_set_style_border_width(telem_ammo_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(telem_ammo_bar, COL_NEON, LV_PART_INDICATOR);
    lv_obj_set_style_radius(telem_ammo_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(telem_ammo_bar, 2, LV_PART_INDICATOR);

    lv_obj_t * note = make_label(ammo_panel, "Secondary counters not exposed by WT API.", COL_DIM, 0, 70);
    lv_obj_set_style_text_color(note, lv_color_hex(0x226622), LV_PART_MAIN);

    // ── RIGHT COLUMN (x=395, w=395) ──────────────────────────────────────────

    // DRIVETRAIN
    lv_obj_t * drive_panel = make_panel(screen_telem, 395, 65, 395, 195, "DRIVETRAIN");
    make_drive_box(drive_panel, "SPEED", &telem_spd,      "KM/H",    0,   26);
    make_drive_box(drive_panel, "RPM",   &telem_rpm,      "ENGINE",  187, 26);
    make_drive_box(drive_panel, "GEAR",  &telem_gear,     "CURRENT", 0,   97);
    make_drive_box(drive_panel, "FUEL",  &telem_fuel_lbl, "%",       187, 97);

    // MODULE INTEGRITY (dynamic — fed by MOD: serial messages)
    lv_obj_t * mod_panel = make_panel(screen_telem, 395, 270, 395, 168, "MODULE INTEGRITY");
    for (int i = 0; i < 6; i++) {
        make_module_bar(mod_panel, MODULE_NAMES[i], i, 26 + i * 22);
    }

    // ── BOTTOM BAR ──────────────────────────────────────────────────────────
    lv_obj_t * bot = lv_obj_create(screen_telem);
    lv_obj_set_size(bot, 800, 38);
    lv_obj_align(bot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bot, lv_color_hex(0x0A1A0A), LV_PART_MAIN);
    lv_obj_set_style_border_color(bot, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_opa(bot, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(bot, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(bot, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * bl = make_label(bot, "TELEMETRY: BRIDGE OFFLINE", COL_DIM, 0, 0);
    lv_obj_align(bl, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t * br = make_label(bot, "STAB: --", COL_DIM, 0, 0);
    lv_obj_align(br, LV_ALIGN_RIGHT_MID, -8, 0);
}

// ============================================================
// SCREEN 3 — TACTICAL MAP (icons + rotation + radar rings)
// ============================================================

static void build_screen_map() {
    lv_obj_set_style_bg_color(screen_map, COL_DARK, LV_PART_MAIN);

    // ── HEADER ──────────────────────────────────────────────────────────────
    lv_obj_t * hdr = lv_obj_create(screen_map);
    lv_obj_set_size(hdr, 800, 55);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x0A1A0A), LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 0, LV_PART_MAIN);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * hdr_sep = lv_obj_create(screen_map);
    lv_obj_set_size(hdr_sep, 800, 2);
    lv_obj_set_pos(hdr_sep, 0, 55);
    lv_obj_set_style_bg_color(hdr_sep, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hdr_sep, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr_sep, 0, LV_PART_MAIN);
    lv_obj_remove_flag(hdr_sep, LV_OBJ_FLAG_SCROLLABLE);

    make_nav_btn(hdr, LV_SYMBOL_LEFT, 8, 8, 50, 38, cb_goto_overview);

    make_label(hdr, "SAT-LINK", COL_DIM, 68, 4);
    make_label(hdr, "TRACKING", COL_NEON, 68, 25);

    lv_obj_t * grid_ttl = make_label(hdr, "TACTICAL GRID // ", COL_DIM, 0, 0);
    lv_obj_align(grid_ttl, LV_ALIGN_RIGHT_MID, -90, 0);

    map_name_label = make_label(hdr, "---", COL_NEON, 0, 0);
    lv_obj_align(map_name_label, LV_ALIGN_RIGHT_MID, -8, 0);

    // ── MAP CONTAINER ───────────────────────────────────────────────────────
    map_container = lv_obj_create(screen_map);
    lv_obj_set_size(map_container, MAP_CONT_W, MAP_CONT_H);
    lv_obj_set_pos(map_container, 30, 64);
    lv_obj_set_style_bg_color(map_container, lv_color_hex(0x060F06), LV_PART_MAIN);
    lv_obj_set_style_border_color(map_container, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_opa(map_container, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(map_container, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(map_container, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(map_container, 0, LV_PART_MAIN);
    lv_obj_remove_flag(map_container, LV_OBJ_FLAG_SCROLLABLE);

    // Background image (lowest z-order)
    map_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    map_img_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    map_img_dsc.header.flags  = 0;
    map_img_dsc.header.w      = MAP_RAW_W;
    map_img_dsc.header.h      = MAP_RAW_H;
    map_img_dsc.header.stride = (uint16_t)(MAP_RAW_W * 2);
    map_img_dsc.data_size     = MAP_RAW_BYTES;
    map_img_dsc.data          = g_map_raw;

    map_bg_img = lv_image_create(map_container);
    lv_image_set_src(map_bg_img, &map_img_dsc);
    lv_obj_set_pos(map_bg_img, 0, 0);
    lv_image_set_pivot(map_bg_img, 0, 0);
    lv_image_set_scale(map_bg_img, MAP_BG_SCALE);
    lv_obj_add_flag(map_bg_img, LV_OBJ_FLAG_HIDDEN);

    // Radar rings (above bg, below icons)
    const int32_t cx = MAP_CONT_W / 2;
    const int32_t cy = MAP_CONT_H / 2;
    const int32_t ring_radii[] = { 50, 100, 150 };
    for (int ri = 0; ri < 3; ri++) {
        int32_t r = ring_radii[ri];
        lv_obj_t * ring = lv_obj_create(map_container);
        lv_obj_set_size(ring, r * 2, r * 2);
        lv_obj_set_pos(ring, cx - r, cy - r);
        lv_obj_set_style_bg_opa(ring, LV_OPA_0, LV_PART_MAIN);
        lv_obj_set_style_border_color(ring, COL_NEON, LV_PART_MAIN);
        lv_obj_set_style_border_opa(ring, LV_OPA_20, LV_PART_MAIN);
        lv_obj_set_style_border_width(ring, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_pad_all(ring, 0, LV_PART_MAIN);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    }

    map_wait_label = lv_label_create(map_container);
    lv_label_set_text(map_wait_label, "En attente de donnees carte...");
    lv_obj_set_style_text_color(map_wait_label, COL_DIM, LV_PART_MAIN);
    lv_obj_set_style_text_font(map_wait_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(map_wait_label);

    // Pre-allocate icon pool (hidden until first update). Each slot is an
    // lv_image whose source descriptor is swapped based on the entity type.
    for (int i = 0; i < MAP_MAX_ENT; i++) {
        map_icons_obj[i] = lv_image_create(map_container);
        lv_image_set_src(map_icons_obj[i], &icon_unknown_dsc);
        lv_image_set_pivot(map_icons_obj[i], MAP_ICON_W / 2, MAP_ICON_H / 2);
        lv_obj_add_flag(map_icons_obj[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(map_icons_obj[i], 0, 0);
    }

    // Legend
    const struct { const char * lbl; lv_color_t col; } LEG[] = {
        { "ALLY",       lv_color_hex(0x00CC00) },
        { "ENEMY",      lv_color_hex(0xFF2200) },
        { "OBJECTIVE",  lv_color_hex(0xFACC15) },
        { "AIRFIELD",   lv_color_hex(0x60A5FA) },
        { "BOMB",       lv_color_hex(0xEF4444) },
        { "RESPAWN",    lv_color_hex(0x396AFA) },
    };
    int32_t lx = 30;
    const int32_t LEG_Y = MAP_CONT_H + 64 + 8;
    for (size_t i = 0; i < sizeof(LEG) / sizeof(LEG[0]); i++) {
        lv_obj_t * dot = lv_obj_create(screen_map);
        lv_obj_set_pos(dot, lx, LEG_Y + 2);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_bg_color(dot, LEG[i].col, LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lx += 14;
        make_label(screen_map, LEG[i].lbl, LEG[i].col, lx, LEG_Y);
        lx += (int32_t)(strlen(LEG[i].lbl) * 8 + 16);
    }
}

// ============================================================
// SCREEN 0 — SPLASH (shown briefly at boot)
// ============================================================

static void build_screen_splash() {
    lv_obj_set_style_bg_color(screen_splash, COL_DARK, LV_PART_MAIN);

    lv_obj_t * logo = lv_image_create(screen_splash);
    lv_image_set_src(logo, &splash_dsc);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t * title = make_label(screen_splash, "GIGA TANK CONTROL HUB", COL_NEON, 0, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 130);

    lv_obj_t * sub = make_label(screen_splash, "V2 // READ-ONLY TELEMETRY HUB", COL_DIM, 0, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 152);

    // Animated load bar
    lv_obj_t * bar = lv_bar_create(screen_splash);
    lv_obj_set_size(bar, 280, 4);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 180);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0A3302), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_NEON, LV_PART_INDICATOR);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_bar_set_value);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 2200);
    lv_anim_start(&a);
}

// ============================================================
// PARSERS — invoked from the serial thread, never call LVGL
// ============================================================

static void parse_telem_cstr(const char * s, size_t len, TelemShared& out) {
    out.spd        = parse_int_field(s, len, "SPD:", 0);
    out.rpm        = parse_int_field(s, len, "RPM:", 0);
    parse_str_field(s, len, "GEAR:", out.gear, sizeof(out.gear), "---");
    out.ammo       = parse_int_field(s, len, "AMMO:", -1);
    out.stab       = parse_int_field(s, len, "STAB:", 0);
    out.fuel       = parse_int_field(s, len, "FUEL:", -1);
    parse_str_field(s, len, "TANK:", out.tank, sizeof(out.tank), "UNKNOWN");
    out.driver     = parse_int_field(s, len, "DRV:",  0);
    out.gunner     = parse_int_field(s, len, "GUN:",  0);
    out.engine_fire= parse_int_field(s, len, "FIRE:", 0);
    out.overspeed  = parse_int_field(s, len, "OVER:", 0);
    out.lws        = parse_int_field(s, len, "LWS:", -1);
    out.ircm       = parse_int_field(s, len, "IRCM:", -1);

    // CREW: "n/total"
    out.crew = 0; out.crew_total = 0;
    const char * pcrew = find_field(s, len, "CREW:");
    if (pcrew) {
        char tmp[16];
        const char * end = strchr(pcrew, '|');
        size_t fl = end ? (size_t)(end - pcrew) : strlen(pcrew);
        if (fl >= sizeof(tmp)) fl = sizeof(tmp) - 1;
        memcpy(tmp, pcrew, fl); tmp[fl] = '\0';
        const char * sl = strchr(tmp, '/');
        if (sl) {
            out.crew       = (int)strtol(tmp, NULL, 10);
            out.crew_total = (int)strtol(sl + 1, NULL, 10);
        }
    }

    const char * pst = find_field(s, len, "STATUS:");
    out.online = pst && pst[0] == '1';
}

// Parses MAPNAME:{name}|MAPOBJ:{x},{y},{type}[,{rot}];...
// All operations are done on the raw char buffer — no String allocation.
static void parse_map_cstr(const char * s, size_t len, MapShared& out) {
    out.count   = 0;
    out.name[0] = '\0';

    parse_str_field(s, len, "MAPNAME:", out.name, sizeof(out.name), "");

    const char * objs = find_field(s, len, "MAPOBJ:");
    if (!objs) return;
    if (objs[0] == '-' || objs[0] == '\0' || objs[0] == '\n') return;

    const char * cur = objs;
    while (out.count < MAP_MAX_ENT && *cur && *cur != '\n' && *cur != '\r') {
        // Skip leading separators
        while (*cur == ';' || *cur == ' ') cur++;
        if (!*cur || *cur == '\n' || *cur == '\r') break;

        const char * end = strchr(cur, ';');
        size_t flen = end ? (size_t)(end - cur) : strlen(cur);

        // Field: x,y,type[,rot]
        char buf[40];
        size_t cp = flen < sizeof(buf) - 1 ? flen : sizeof(buf) - 1;
        memcpy(buf, cur, cp); buf[cp] = '\0';

        char * c1 = strchr(buf, ',');
        if (!c1) break;
        char * c2 = strchr(c1 + 1, ',');
        if (!c2) break;
        char * c3 = strchr(c2 + 1, ',');

        *c1 = '\0'; *c2 = '\0';
        if (c3) *c3 = '\0';

        out.ents[out.count].x       = (float)atof(buf);
        out.ents[out.count].y       = (float)atof(c1 + 1);
        out.ents[out.count].type    = *(c2 + 1);
        out.ents[out.count].rot_deg = c3 ? (int16_t)atoi(c3 + 1) : 0;
        out.count++;

        if (!end) break;
        cur = end + 1;
    }
}

// Parses MOD:ENG:100|TRANS:80|TURR:100|GUN:90|TRKL:60|TRKR:100
static void parse_modules_cstr(const char * s, size_t len, ModulesShared& out) {
    out.engine       = (uint8_t)parse_int_field(s, len, "ENG:",   100);
    out.transmission = (uint8_t)parse_int_field(s, len, "TRANS:", 100);
    out.turret       = (uint8_t)parse_int_field(s, len, "TURR:",  100);
    out.barrel       = (uint8_t)parse_int_field(s, len, "GUN:",   100);
    out.track_l      = (uint8_t)parse_int_field(s, len, "TRKL:",  100);
    out.track_r      = (uint8_t)parse_int_field(s, len, "TRKR:",  100);
    out.valid        = true;
}

// Parses EVT:{kind}:{msg} — pushes one event into the ring buffer.
// Caller must hold g_data_mutex.
static void push_event_locked(char kind, const char * msg) {
    EventShared & e = g_events[g_events_head];
    e.kind   = kind;
    e.age_ms = (uint32_t)millis();
    e.valid  = true;
    size_t L = strlen(msg);
    if (L >= EVENT_MSG_LEN) L = EVENT_MSG_LEN - 1;
    memcpy(e.msg, msg, L); e.msg[L] = '\0';
    g_events_head = (g_events_head + 1) % EVENT_MAX;
}

// ============================================================
// LVGL UPDATE FUNCTIONS — main loop only
// ============================================================

static void apply_telem_update(const TelemShared& d) {
    char buf[96];

    // Header HUD line
    snprintf(buf, sizeof(buf), "%.32s | %d km/h | CREW:%d/%d",
             d.tank, d.spd, d.crew, d.crew_total);
    lv_label_set_text(hud_label_btn, buf);

    if (hud_bridge_lbl) {
        if (d.online) {
            lv_label_set_text(hud_bridge_lbl, "ACTIVE");
            lv_obj_set_style_text_color(hud_bridge_lbl, COL_NEON, LV_PART_MAIN);
        } else {
            lv_label_set_text(hud_bridge_lbl, "OFFLINE");
            lv_obj_set_style_text_color(hud_bridge_lbl, COL_DANGER, LV_PART_MAIN);
        }
    }

    // ── Status screen (screen 2) ────────────────────────────────────────────
    if (telem_status) {
        if (d.online) {
            lv_label_set_text(telem_status, "LIVE");
            lv_obj_set_style_text_color(telem_status, COL_NEON, LV_PART_MAIN);
        } else {
            lv_label_set_text(telem_status, "BRIDGE OFFLINE");
            lv_obj_set_style_text_color(telem_status, COL_DANGER, LV_PART_MAIN);
        }
    }

    if (telem_spd)  { snprintf(buf, sizeof(buf), "%d", d.spd); lv_label_set_text(telem_spd, buf); }
    if (telem_rpm)  { snprintf(buf, sizeof(buf), "%d", d.rpm); lv_label_set_text(telem_rpm, buf); }
    if (telem_gear) lv_label_set_text(telem_gear, d.gear);

    if (telem_fuel_lbl) {
        if (d.fuel >= 0) {
            snprintf(buf, sizeof(buf), "%d", d.fuel);
            lv_label_set_text(telem_fuel_lbl, buf);
            lv_obj_set_style_text_color(telem_fuel_lbl,
                d.fuel < 20 ? COL_DANGER : COL_NEON, LV_PART_MAIN);
        } else {
            lv_label_set_text(telem_fuel_lbl, "N/A");
        }
    }

    if (telem_crew_count) {
        snprintf(buf, sizeof(buf), "%d/%d", d.crew, d.crew_total);
        lv_label_set_text(telem_crew_count, buf);
        lv_color_t cnt_col = (d.crew_total > 0 && d.crew < d.crew_total)
                             ? COL_DANGER : COL_NEON;
        lv_obj_set_style_text_color(telem_crew_count, cnt_col, LV_PART_MAIN);
    }
    if (telem_crew_bar) {
        int pct = (d.crew_total > 0) ? (d.crew * 100 / d.crew_total) : 0;
        lv_bar_set_value(telem_crew_bar, pct, LV_ANIM_OFF);
        lv_color_t bar_col = (pct > 50) ? COL_NEON
                           : (pct > 25) ? COL_WARN
                                        : COL_DANGER;
        lv_obj_set_style_bg_color(telem_crew_bar, bar_col, LV_PART_INDICATOR);
    }

    if (telem_ammo_lbl) {
        if (d.ammo >= 0) {
            snprintf(buf, sizeof(buf), "%d", d.ammo);
            lv_label_set_text(telem_ammo_lbl, buf);
            lv_obj_set_style_text_color(telem_ammo_lbl,
                d.ammo < 5 ? COL_DANGER : COL_NEON, LV_PART_MAIN);
        } else {
            lv_label_set_text(telem_ammo_lbl, "--");
        }
    }
    if (telem_ammo_bar && d.ammo >= 0) {
        lv_bar_set_value(telem_ammo_bar, d.ammo, LV_ANIM_OFF);
        lv_color_t a_col = (d.ammo > 10) ? COL_NEON
                         : (d.ammo > 5)  ? COL_WARN
                                         : COL_DANGER;
        lv_obj_set_style_bg_color(telem_ammo_bar, a_col, LV_PART_INDICATOR);
    }

    // ── Overview screen (screen 1) ──────────────────────────────────────────
    if (ov_speed_val) { snprintf(buf, sizeof(buf), "%d", d.spd); lv_label_set_text(ov_speed_val, buf); }
    if (ov_rpm_val)   { snprintf(buf, sizeof(buf), "%d", d.rpm); lv_label_set_text(ov_rpm_val, buf); }
    if (ov_rpm_bar)   lv_bar_set_value(ov_rpm_bar, d.rpm > 3000 ? 3000 : d.rpm, LV_ANIM_OFF);
    if (ov_gear_val)  lv_label_set_text(ov_gear_val, d.gear);
    if (ov_fuel_val) {
        if (d.fuel >= 0) {
            snprintf(buf, sizeof(buf), "%d %%", d.fuel);
            lv_label_set_text(ov_fuel_val, buf);
            lv_obj_set_style_text_color(ov_fuel_val,
                d.fuel < 20 ? COL_DANGER : COL_NEON, LV_PART_MAIN);
        } else {
            lv_label_set_text(ov_fuel_val, "N/A");
        }
    }
    if (ov_fuel_bar && d.fuel >= 0) {
        lv_bar_set_value(ov_fuel_bar, d.fuel, LV_ANIM_OFF);
        lv_color_t f_col = (d.fuel > 40) ? COL_NEON
                         : (d.fuel > 15) ? COL_WARN
                                         : COL_DANGER;
        lv_obj_set_style_bg_color(ov_fuel_bar, f_col, LV_PART_INDICATOR);
    }

    if (ov_crew_val) {
        snprintf(buf, sizeof(buf), "%d/%d", d.crew, d.crew_total);
        lv_label_set_text(ov_crew_val, buf);
        lv_color_t c_col = (d.crew_total > 0 && d.crew < d.crew_total)
                           ? COL_DANGER : COL_NEON;
        lv_obj_set_style_text_color(ov_crew_val, c_col, LV_PART_MAIN);
    }
    if (ov_crew_bar) {
        int pct = (d.crew_total > 0) ? (d.crew * 100 / d.crew_total) : 0;
        lv_bar_set_value(ov_crew_bar, pct, LV_ANIM_OFF);
        lv_color_t c_col = (pct > 50) ? COL_NEON : (pct > 25) ? COL_WARN : COL_DANGER;
        lv_obj_set_style_bg_color(ov_crew_bar, c_col, LV_PART_INDICATOR);
    }

    if (ov_driver_val) {
        bool down = (d.driver != 0);
        lv_label_set_text(ov_driver_val, down ? "DOWN" : "OK");
        lv_obj_set_style_text_color(ov_driver_val, down ? COL_DANGER : COL_NEON, LV_PART_MAIN);
    }
    if (ov_gunner_val) {
        bool down = (d.gunner != 0);
        lv_label_set_text(ov_gunner_val, down ? "DOWN" : "OK");
        lv_obj_set_style_text_color(ov_gunner_val, down ? COL_DANGER : COL_NEON, LV_PART_MAIN);
    }
    if (ov_stab_val) {
        bool on = (d.stab != 0);
        lv_label_set_text(ov_stab_val, on ? "ARMED" : "OFF");
        lv_obj_set_style_text_color(ov_stab_val, on ? COL_NEON : COL_DIM, LV_PART_MAIN);
    }

    // Warnings (active = red & bright, inactive = dim)
    auto set_warn = [](lv_obj_t * lbl, bool active) {
        if (!lbl) return;
        lv_obj_set_style_text_color(lbl, active ? COL_DANGER : COL_DIM, LV_PART_MAIN);
    };
    set_warn(ov_warn_fire, d.engine_fire != 0);
    set_warn(ov_warn_over, d.overspeed != 0);
    set_warn(ov_warn_lws,  d.lws > 0);
    set_warn(ov_warn_ircm, d.ircm > 0);
    set_warn(ov_warn_ammo, d.ammo >= 0 && d.ammo < 5);
    set_warn(ov_warn_fuel, d.fuel >= 0 && d.fuel < 20);
}

static void apply_modules_update(const ModulesShared& m) {
    if (!m.valid) return;
    const uint8_t vals[6] = {
        m.engine, m.transmission, m.turret, m.barrel, m.track_l, m.track_r
    };
    char buf[8];
    for (int i = 0; i < 6; i++) {
        if (mod_widgets[i].bar) {
            lv_bar_set_value(mod_widgets[i].bar, vals[i], LV_ANIM_OFF);
            lv_color_t col = (vals[i] > 50) ? COL_NEON
                           : (vals[i] > 25) ? COL_WARN
                                            : COL_DANGER;
            lv_obj_set_style_bg_color(mod_widgets[i].bar, col, LV_PART_INDICATOR);
        }
        if (mod_widgets[i].pct) {
            snprintf(buf, sizeof(buf), "%u%%", (unsigned)vals[i]);
            lv_label_set_text(mod_widgets[i].pct, buf);
        }
    }
}

static void apply_events_update() {
    // Take a local snapshot under mutex, then render outside.
    EventShared local[EVENT_MAX];
    int head;
    g_data_mutex.lock();
    memcpy(local, g_events, sizeof(local));
    head = g_events_head;
    g_data_mutex.unlock();

    // Render most-recent first (slot just before head)
    for (int i = 0; i < EVENT_MAX; i++) {
        int idx = (head - 1 - i + EVENT_MAX) % EVENT_MAX;
        const EventShared & e = local[idx];
        if (!ov_event_labels[i]) continue;
        if (!e.valid) {
            lv_label_set_text(ov_event_labels[i], "");
            continue;
        }
        char line[96];
        uint32_t now    = (uint32_t)millis();
        uint32_t age_s  = (now - e.age_ms) / 1000U;
        char prefix     = (e.kind == 'K') ? '+' : (e.kind == 'D') ? '!' : '*';
        snprintf(line, sizeof(line), "%c [%lus] %s", prefix, (unsigned long)age_s, e.msg);
        lv_label_set_text(ov_event_labels[i], line);
        lv_color_t col = (e.kind == 'K') ? COL_NEON
                       : (e.kind == 'D') ? COL_DANGER
                                         : COL_WARN;
        lv_obj_set_style_text_color(ov_event_labels[i], col, LV_PART_MAIN);
    }
}

static void apply_map_update(const MapShared& d) {
    if (map_name_label) lv_label_set_text(map_name_label, d.name);

    for (int i = 0; i < MAP_MAX_ENT; i++) {
        lv_obj_add_flag(map_icons_obj[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (d.count == 0) return;

    if (map_wait_label) lv_obj_add_flag(map_wait_label, LV_OBJ_FLAG_HIDDEN);

    const int32_t max_px = MAP_CONT_W - MAP_ICON_W;
    const int32_t max_py = MAP_CONT_H - MAP_ICON_H;

    for (int i = 0; i < d.count && i < MAP_MAX_ENT; i++) {
        int32_t px = (int32_t)(d.ents[i].x * max_px);
        int32_t py = (int32_t)(d.ents[i].y * max_py);
        if (px < 0) px = 0; else if (px > max_px) px = max_px;
        if (py < 0) py = 0; else if (py > max_py) py = max_py;

        lv_image_set_src(map_icons_obj[i], icon_for_type(d.ents[i].type));
        lv_image_set_rotation(map_icons_obj[i], (int16_t)(d.ents[i].rot_deg * 10));
        lv_obj_set_pos(map_icons_obj[i], px, py);
        lv_obj_remove_flag(map_icons_obj[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_map_image() {
    if (map_bg_img == NULL) return;
    lv_image_set_src(map_bg_img, &map_img_dsc);
    lv_obj_remove_flag(map_bg_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(map_bg_img);
}

// ============================================================
// SERIAL THREAD — fixed char buffers, no String allocations
// ============================================================

void serial_task() {
    // Fixed-size text accumulator (no heap allocations).
    static char   text_buf[512];
    static size_t text_len   = 0;
    static bool   is_mapraw  = false;

    // Buffers for the base64 image payload (BSS — does not eat the thread stack).
    static uint8_t s_b64buf[MAP_B64_MAX];
    static size_t  s_b64len   = 0;
    // Temporary RGB565 buffer so the final copy into g_map_raw is atomic
    // w.r.t. the M7 main loop (which renders g_map_raw without the mutex).
    static uint8_t s_decode_tmp[MAP_RAW_BYTES];

    while (true) {
        while (Serial.available()) {
            char c = (char)Serial.read();

            if (c == '\n') {
                if (is_mapraw) {
                    uint32_t decoded = b64_decode_buf(s_b64buf, s_b64len,
                                                     s_decode_tmp, MAP_RAW_BYTES);
                    if (decoded == MAP_RAW_BYTES) {
                        g_data_mutex.lock();
                        memcpy(g_map_raw, s_decode_tmp, MAP_RAW_BYTES);
                        g_img_updated = true;
                        g_data_mutex.unlock();
                    }
                    is_mapraw = false;
                    s_b64len  = 0;
                } else if (text_len > 0) {
                    // Trim trailing \r
                    while (text_len > 0 && (text_buf[text_len - 1] == '\r' || text_buf[text_len - 1] == ' ')) {
                        text_buf[--text_len] = '\0';
                    }
                    text_buf[text_len] = '\0';

                    if (text_len >= 8 && memcmp(text_buf, "MAPNAME:", 8) == 0) {
                        MapShared local;
                        parse_map_cstr(text_buf, text_len, local);
                        g_data_mutex.lock();
                        g_map         = local;
                        g_map_updated = true;
                        g_data_mutex.unlock();
                    } else if (text_len >= 4 && memcmp(text_buf, "MOD:", 4) == 0) {
                        ModulesShared local;
                        parse_modules_cstr(text_buf, text_len, local);
                        g_data_mutex.lock();
                        g_modules         = local;
                        g_modules_updated = true;
                        g_data_mutex.unlock();
                    } else if (text_len >= 4 && memcmp(text_buf, "EVT:", 4) == 0) {
                        // EVT:K:msg or EVT:D:msg or EVT:A:msg
                        char kind   = (text_len >= 6) ? text_buf[4] : 'A';
                        const char * msg_start = (text_len >= 6 && text_buf[5] == ':') ? &text_buf[6] : "";
                        g_data_mutex.lock();
                        push_event_locked(kind, msg_start);
                        g_events_updated = true;
                        g_data_mutex.unlock();
                    } else {
                        TelemShared local;
                        parse_telem_cstr(text_buf, text_len, local);
                        g_data_mutex.lock();
                        g_telem         = local;
                        g_telem_updated = true;
                        g_data_mutex.unlock();
                    }
                    text_len = 0;
                }
            } else {
                if (is_mapraw) {
                    if (s_b64len < MAP_B64_MAX - 1)
                        s_b64buf[s_b64len++] = (uint8_t)c;
                    // else: silently drop overflow
                } else {
                    if (text_len < sizeof(text_buf) - 1) {
                        text_buf[text_len++] = c;
                        // Detect MAPRAW: prefix as soon as it is complete
                        if (text_len == 7 && memcmp(text_buf, "MAPRAW:", 7) == 0) {
                            is_mapraw = true;
                            s_b64len  = 0;
                            text_len  = 0;
                        }
                    } else {
                        // Drop runaway frame
                        text_len = 0;
                    }
                }
            }
        }
        // Yield 2 ms so the M7 loop and USB stack get CPU time
        delay(2);
    }
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
    Serial.begin(115200);
    Display.begin();
    TouchDetector.begin();

    COL_NEON   = lv_color_hex(0x39FF14);
    COL_DANGER = lv_color_hex(0xEF4444);
    COL_WARN   = lv_color_hex(0xFACC15);
    COL_DARK   = lv_color_hex(0x051005);
    COL_BAR    = lv_color_hex(0x0A1A0A);
    COL_DIM    = lv_color_hex(0x336633);

    // Splash first — shown alone for ~2.5 s, then transitions to overview.
    screen_splash   = lv_obj_create(NULL);
    screen_overview = lv_obj_create(NULL);
    screen_telem    = lv_obj_create(NULL);
    screen_map      = lv_obj_create(NULL);

    build_screen_splash();
    build_screen_overview();
    build_screen_telem();
    build_screen_map();

    lv_screen_load(screen_splash);

    // Pump LVGL for ~2.5 s so the load bar animation runs.
    uint32_t splash_start = millis();
    while (millis() - splash_start < 2500U) {
        lv_timer_handler();
        delay(5);
    }

    lv_screen_load_anim(screen_overview, LV_SCR_LOAD_ANIM_FADE_IN, 400, 0, true);
    screen_splash = NULL;   // released by LVGL after the animation completes

    // Initialise shared state so the first render shows clean defaults.
    g_modules.engine = g_modules.transmission = g_modules.turret =
    g_modules.barrel = g_modules.track_l = g_modules.track_r = 100;
    g_modules.valid  = false;
    for (int i = 0; i < EVENT_MAX; i++) g_events[i].valid = false;

    g_serial_thread.start(serial_task);
}

void loop() {
    static TelemShared    local_telem;
    static MapShared      local_map;
    static ModulesShared  local_mods;
    bool do_telem = false, do_map = false, do_img = false, do_mods = false, do_events = false;

    g_data_mutex.lock();
    if (g_telem_updated)   { local_telem = g_telem;   do_telem = true;   g_telem_updated   = false; }
    if (g_map_updated)     { local_map   = g_map;     do_map   = true;   g_map_updated     = false; }
    if (g_img_updated)     {                          do_img   = true;   g_img_updated     = false; }
    if (g_modules_updated) { local_mods  = g_modules; do_mods  = true;   g_modules_updated = false; }
    if (g_events_updated)  {                          do_events= true;   g_events_updated  = false; }
    g_data_mutex.unlock();

    if (do_telem)  apply_telem_update(local_telem);
    if (do_map)    apply_map_update(local_map);
    if (do_img)    apply_map_image();
    if (do_mods)   apply_modules_update(local_mods);
    if (do_events) apply_events_update();

    // Clock (elapsed since boot — no RTC on the GIGA)
    if (hud_time_lbl) {
        static unsigned long last_tick = 0;
        unsigned long now = millis();
        if (now - last_tick >= 1000UL) {
            last_tick = now;
            unsigned long secs = now / 1000UL;
            char tbuf[12];
            snprintf(tbuf, sizeof(tbuf), "%02lu:%02lu:%02lu",
                     secs / 3600UL, (secs % 3600UL) / 60UL, secs % 60UL);
            lv_label_set_text(hud_time_lbl, tbuf);
        }
    }

    lv_timer_handler();

    // 1 ms yield — no USB-HID stack to flush anymore, so the loop can run faster
    // (~500 Hz max), giving smoother radar sweep + bar animations.
    delay(1);
}
