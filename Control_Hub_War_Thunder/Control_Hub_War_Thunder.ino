// ============================================================
// Control_Hub_War_Thunder — Cortex-M7 main sketch
// Target core : Main Core  (Tools → Target Core → Main Core)
//
// Dual-core overview:
//   This sketch runs on the M7 and owns the display (LVGL), the USB HID
//   keyboard, and the USB Serial connection to the Python bridge.
//   It boots the M4 co-processor via RPC.begin() and delegates all
//   telemetry parsing to it:
//
//     1. Raw telemetry lines from Serial (Python) are forwarded to the M4
//        with RPC.println().
//     2. The M4 parses each line and sends back a compact "PARSED|…" frame
//        over the same bidirectional RPC stream.
//     3. apply_parsed() reads those frames and updates the LVGL widgets.
//
//   See Control_Hub_M4/Control_Hub_M4.ino for the co-processor sketch.
//
// Flash split (Arduino IDE → Tools → Flash Split):
//   "1MB M7 + 1MB M4"  or  "1.5MB M7 + 0.5MB M4"
//   Upload the M7 sketch first, then switch to M4 Co-processor and upload
//   Control_Hub_M4.ino.
// ============================================================

#include "Arduino_H7_Video.h"
#include "Arduino_GigaDisplayTouch.h"
#include "lvgl.h"
#include "PluggableUSBHID.h"
#include "USBKeyboard.h"
#include "mbed.h"
#include "RPC.h"

// ===== HARDWARE =====
Arduino_H7_Video Display(800, 480, GigaDisplayShield);
Arduino_GigaDisplayTouch TouchDetector;
USBKeyboard Keyboard;

// ===== ECRANS =====
lv_obj_t * screen_buttons;
lv_obj_t * screen_telem;
lv_obj_t * screen_map;

// ===== WIDGETS GLOBAUX =====
lv_obj_t * hud_label_btn;
lv_obj_t * telem_spd;
lv_obj_t * telem_rpm;
lv_obj_t * telem_gear;
lv_obj_t * telem_raw;
lv_obj_t * telem_status;

// ===== CARTE =====
// Maximum number of map entities displayed simultaneously.
#define MAP_MAX_ENT  20
// Dot size in pixels (circle diameter).
#define MAP_ENT_SIZE 12
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

// Assure la compatibilite si LV_IMAGE_HEADER_MAGIC n'est pas expose par la version Arduino de LVGL.
#ifndef LV_IMAGE_HEADER_MAGIC
#  define LV_IMAGE_HEADER_MAGIC  0x19U
#endif

static lv_obj_t * map_name_label;
static lv_obj_t * map_wait_label;
static lv_obj_t * map_container;
static lv_obj_t * map_dots[MAP_MAX_ENT];

// Tampon RGB565 pour l'image d'arriere-plan (ecrit par le thread serie, lu par le thread M7).
static uint8_t        g_map_raw[MAP_RAW_BYTES];
// Descripteur d'image LVGL pointant en permanence sur g_map_raw.
static lv_image_dsc_t map_img_dsc;
// Widget image d'arriere-plan (cree et utilise dans le thread M7 uniquement).
static lv_obj_t     * map_bg_img    = NULL;
// Drapeau : g_map_raw contient de nouveaux pixels a afficher (protege par g_data_mutex).
static volatile bool  g_img_updated = false;

// ===== DONNEES PARTAGEES (thread serial <-> thread LVGL/M7) =====
// All fields written by the serial thread and read by the main loop.
// Access is protected by g_data_mutex. LVGL is NEVER called from the
// serial thread; it only writes plain POD data to these structs.

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
    char raw[256];   // full raw line for the RAW DATA STREAM label
};

struct MapEntShared {
    float x;
    float y;
    char  type;      // 'A'=allié 'E'=ennemi 'O'=objectif 'F'=aerodrome 'N'=autre
};

struct MapShared {
    char         name[16];
    MapEntShared ents[MAP_MAX_ENT];
    int          count;
};

static TelemShared   g_telem;
static MapShared     g_map;
static rtos::Mutex   g_data_mutex;           // guards g_telem + g_map
static volatile bool g_telem_updated = false;
static volatile bool g_map_updated   = false;

// Serial-reader thread (Mbed RTOS — M4-equivalent role).
// 8 KB stack fits Arduino String temporaries and parse buffers.
static rtos::Thread g_serial_thread(osPriorityHigh, 8192);

// ===== WIDGETS VEHICULE STATUS (ecran 2) =====
static lv_obj_t * telem_crew_bar    = NULL;  // barre progression equipage
static lv_obj_t * telem_crew_count  = NULL;  // label "3/4"
static lv_obj_t * telem_ammo_lbl    = NULL;  // label compteur munitions
static lv_obj_t * telem_ammo_bar    = NULL;  // barre munitions
static lv_obj_t * telem_fuel_lbl    = NULL;  // label carburant
// ===== WIDGETS HUD ECRAN PRINCIPAL =====
static lv_obj_t * hud_bridge_lbl    = NULL;  // statut bridge (ecran boutons)
static lv_obj_t * hud_time_lbl      = NULL;  // horloge HH:MM:SS

// ===== COULEURS =====
lv_color_t COL_DANGER;
lv_color_t COL_ARMOR;
lv_color_t COL_TECH;
lv_color_t COL_DARK;
lv_color_t COL_BAR;

// ===== DEFERRED HID DISPATCH =====
// Callbacks write here; loop() sends after lv_timer_handler() so the USB
// stack has a clean 5 ms window to flush before the next frame.
// '\x01' = sentinel for KEY_SHIFT (no printable char).
static volatile char g_pending_hid = '\0';

// ===== CALLBACKS HID =====
static void cb_extincteur(lv_event_t * e)  { g_pending_hid = '6'; }
static void cb_fumigene(lv_event_t * e)    { g_pending_hid = 'g'; }
static void cb_artillerie(lv_event_t * e)  { g_pending_hid = '5'; }
static void cb_jumelles(lv_event_t * e)    { g_pending_hid = 'b'; }
static void cb_sniper(lv_event_t * e)      { g_pending_hid = '\x01'; }
static void cb_moteur(lv_event_t * e)      { g_pending_hid = 'i'; }
static void cb_reparation(lv_event_t * e)  { g_pending_hid = 'f'; }

// ===== SWITCH D'ECRANS =====
// LVGL 9: lv_scr_load_anim -> lv_screen_load_anim
static void cb_goto_telem(lv_event_t * e) {
    lv_screen_load_anim(screen_telem, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}
static void cb_goto_buttons(lv_event_t * e) {
    lv_screen_load_anim(screen_buttons, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}
static void cb_goto_map(lv_event_t * e) {
    lv_screen_load_anim(screen_map, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

// ===== FEEDBACK VISUEL =====
// Registered only for LV_EVENT_PRESSED / LV_EVENT_RELEASED / LV_EVENT_PRESS_LOST
// (targeted registration avoids dispatching every LVGL internal event to this callback)
static void btn_visual_cb(lv_event_t * e) {
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xFF6600), LV_PART_MAIN);
    } else {
        lv_color_t * base = (lv_color_t *)lv_event_get_user_data(e);
        lv_obj_set_style_bg_color(btn, *base, LV_PART_MAIN);
    }
}

// ===== CREATION BOUTON MFD (style Figma) =====
// is_alert=true  → bordure/texte rouge  (SMOKE, ARTILLERIE)
// is_alert=false → bordure/texte vert neon
void make_btn(lv_obj_t * parent, const char* icon, const char* label,
              int32_t x, int32_t y, int32_t w, int32_t h,
              lv_event_cb_t hid_cb, bool is_alert) {

    lv_color_t fg  = is_alert ? lv_color_hex(0xEF4444) : COL_NEON;
    lv_color_t bg  = is_alert ? lv_color_hex(0x1A0000) : lv_color_hex(0x0A1A0A);
    lv_color_t prb = is_alert ? lv_color_hex(0x4A0000) : lv_color_hex(0x103310);

    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, bg,  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, prb, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, fg, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, hid_cb, LV_EVENT_PRESSED, NULL);

    lv_obj_t * ico = lv_label_create(btn);
    lv_label_set_text(ico, icon);
    lv_obj_set_style_text_color(ico, fg, LV_PART_MAIN);
    lv_obj_set_style_text_font(ico, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ico, LV_ALIGN_CENTER, 0, -18);

    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, fg, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 30);
}

// ===== BARRE HUD =====
void make_hud_bar(lv_obj_t * parent, lv_obj_t ** label_out) {
    lv_obj_t * bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 800, 50);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, COL_BAR, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
    // LVGL 9: lv_obj_clear_flag -> lv_obj_remove_flag
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    *label_out = lv_label_create(bar);
    lv_label_set_text(*label_out, "AWAITING TELEMETRY...");
    lv_obj_set_style_text_color(*label_out, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(*label_out, &lv_font_montserrat_14, LV_PART_MAIN);
    // Cap width so the label doesn't overlap the two nav buttons on the right.
    lv_obj_set_width(*label_out, 465);
    lv_label_set_long_mode(*label_out, LV_LABEL_LONG_DOT);
    lv_obj_align(*label_out, LV_ALIGN_LEFT_MID, 10, 0);
}

// ===== DECODAGE BASE64 (thread serie) =====
// Decodage autonome, sans librairie externe.
// Retourne le nombre d'octets ecrits dans dst, ou 0 si src_len == 0.
static uint32_t b64_decode_buf(const uint8_t * src, size_t src_len,
                                uint8_t * dst,       size_t dst_max) {
    // Table de valeurs : -1 = invalide, -2 = padding ('='), 0-63 = valeur base64.
    static const int8_t T[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 0x00 */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 0x10 */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,  /* 0x20  +  / */
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,  /* 0x30  0-9  = */
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, /* 0x40  A-O */
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,  /* 0x50  P-Z */
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, /* 0x60  a-o */
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,  /* 0x70  p-z */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 0x80 */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 0x90 */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 0xA0 */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 0xB0 */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 0xC0 */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 0xD0 */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  /* 0xE0 */
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1   /* 0xF0 */
    };
    uint32_t out  = 0;
    int      acc  = 0;
    int      bits = 0;
    for (size_t i = 0; i < src_len; i++) {
        int v = T[(uint8_t)src[i]];
        if (v < 0) { if (v == -2) break; continue; }  // Ignore invalide, stop sur '='
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

// ===== COULEUR ENTITE CARTE =====
static lv_color_t color_for_type(char t) {
    switch (t) {
        case 'A': return lv_color_hex(0x00CC00);  // Vert  - allié
        case 'E': return lv_color_hex(0xFF2200);  // Rouge - ennemi
        case 'O': return lv_color_hex(0xFFCC00);  // Jaune - objectif
        case 'F': return lv_color_hex(0x555555);  // Gris  - aerodrome
        default:  return lv_color_hex(0x777777);  // Gris clair - inconnu
    }
}

// ===== ECRAN 1 : TABLEAU DE BORD =====
void build_screen_buttons() {
    lv_obj_set_style_bg_color(screen_buttons, COL_DARK, LV_PART_MAIN);

    // ── EN-TETE (57 px) ──────────────────────────────────────────────────────
    lv_obj_t * hdr = lv_obj_create(screen_buttons);
    lv_obj_set_size(hdr, 800, 57);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x0A1A0A), LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(hdr, 0, LV_PART_MAIN);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    // Ligne separatrice inferieure de l'en-tete
    lv_obj_t * hdr_sep = lv_obj_create(screen_buttons);
    lv_obj_set_size(hdr_sep, 800, 2);
    lv_obj_set_pos(hdr_sep, 0, 57);
    lv_obj_set_style_bg_color(hdr_sep, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(hdr_sep, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_border_width(hdr_sep, 0, LV_PART_MAIN);
    lv_obj_remove_flag(hdr_sep, LV_OBJ_FLAG_SCROLLABLE);

    // "SYSTEM" etiquette
    lv_obj_t * sys_ttl = lv_label_create(hdr);
    lv_label_set_text(sys_ttl, "SYSTEM");
    lv_obj_set_style_text_color(sys_ttl, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(sys_ttl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(sys_ttl, 8, 4);

    // "ONLINE" valeur
    lv_obj_t * sys_val = lv_label_create(hdr);
    lv_label_set_text(sys_val, "ONLINE");
    lv_obj_set_style_text_color(sys_val, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(sys_val, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(sys_val, 8, 25);

    // "BRIDGE" etiquette
    lv_obj_t * brg_ttl = lv_label_create(hdr);
    lv_label_set_text(brg_ttl, "BRIDGE");
    lv_obj_set_style_text_color(brg_ttl, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(brg_ttl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(brg_ttl, 115, 4);

    // Statut bridge (mis a jour dynamiquement)
    hud_bridge_lbl = lv_label_create(hdr);
    lv_label_set_text(hud_bridge_lbl, "OFFLINE");
    lv_obj_set_style_text_color(hud_bridge_lbl, lv_color_hex(0xFF3300), LV_PART_MAIN);
    lv_obj_set_style_text_font(hud_bridge_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(hud_bridge_lbl, 115, 25);

    // Info HUD tank (mis a jour dynamiquement)
    hud_label_btn = lv_label_create(hdr);
    lv_label_set_text(hud_label_btn, "AWAITING TELEMETRY...");
    lv_obj_set_style_text_color(hud_label_btn, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(hud_label_btn, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_width(hud_label_btn, 215);
    lv_label_set_long_mode(hud_label_btn, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(hud_label_btn, 228, 14);

    // Bouton TACTICAL MAP
    lv_obj_t * btn_map = lv_btn_create(hdr);
    lv_obj_set_pos(btn_map, 452, 9);
    lv_obj_set_size(btn_map, 143, 38);
    lv_obj_set_style_bg_color(btn_map, lv_color_hex(0x001835), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_map, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_map, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_map, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_map, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_map, cb_goto_map, LV_EVENT_CLICKED, NULL);
    lv_obj_t * map_lbl = lv_label_create(btn_map);
    lv_label_set_text(map_lbl, LV_SYMBOL_GPS " TACTICAL MAP");
    lv_obj_set_style_text_color(map_lbl, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(map_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(map_lbl);

    // Bouton SYS STATUS
    lv_obj_t * btn_telem = lv_btn_create(hdr);
    lv_obj_set_pos(btn_telem, 603, 9);
    lv_obj_set_size(btn_telem, 130, 38);
    lv_obj_set_style_bg_color(btn_telem, lv_color_hex(0x001835), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_telem, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_telem, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_telem, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_telem, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_telem, cb_goto_telem, LV_EVENT_CLICKED, NULL);
    lv_obj_t * telem_nav_lbl = lv_label_create(btn_telem);
    lv_label_set_text(telem_nav_lbl, LV_SYMBOL_RIGHT " SYS STATUS");
    lv_obj_set_style_text_color(telem_nav_lbl, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(telem_nav_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(telem_nav_lbl);

    // Horloge (temps ecoule depuis le demarrage)
    hud_time_lbl = lv_label_create(hdr);
    lv_label_set_text(hud_time_lbl, "00:00:00");
    lv_obj_set_style_text_color(hud_time_lbl, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(hud_time_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(hud_time_lbl, LV_ALIGN_RIGHT_MID, -6, 0);

    // ── 10 BOUTONS MFD — grille 5 × 2 ───────────────────────────────────────
    // Dimensions : BW=150 BH=175, GAP=8, MARG=10
    // Col X : 10, 168, 326, 484, 642   (fin col 5 = 792)
    // Row Y : 67 (row1), 250 (row2)     (fin row2 = 425)
    const int32_t BW = 150, BH = 175, GAP = 8, MARG = 10;
    const int32_t Y1 = 67,  Y2 = Y1 + BH + GAP;
    const int32_t X1 = MARG;
    const int32_t X2 = X1 + BW + GAP;
    const int32_t X3 = X2 + BW + GAP;
    const int32_t X4 = X3 + BW + GAP;
    const int32_t X5 = X4 + BW + GAP;

    // Ligne 1 — commandes courantes
    make_btn(screen_buttons, LV_SYMBOL_POWER,    "ENGINE [I]",     X1, Y1, BW, BH, cb_moteur,     false);
    make_btn(screen_buttons, LV_SYMBOL_WIFI,     "SMOKE [G]",      X2, Y1, BW, BH, cb_fumigene,   true);
    make_btn(screen_buttons, LV_SYMBOL_GPS,      "ARTILLERY [5]",  X3, Y1, BW, BH, cb_artillerie, true);
    make_btn(screen_buttons, LV_SYMBOL_WARNING,  "EXTINGUISHER[6]",X4, Y1, BW, BH, cb_extincteur, false);
    make_btn(screen_buttons, LV_SYMBOL_IMAGE,    "THERMAL [N]",    X5, Y1, BW, BH, cb_thermal,    false);

    // Ligne 2 — optiques et capacites
    make_btn(screen_buttons, LV_SYMBOL_VIDEO,    "BINOCULARS [B]", X1, Y2, BW, BH, cb_jumelles,    false);
    make_btn(screen_buttons, LV_SYMBOL_SETTINGS, "RANGEFINDER[R]", X2, Y2, BW, BH, cb_rangefinder, false);
    make_btn(screen_buttons, LV_SYMBOL_STOP,     "TRACK TGT [X]",  X3, Y2, BW, BH, cb_track,       false);
    make_btn(screen_buttons, LV_SYMBOL_EDIT,     "TOW CABLE [0]",  X4, Y2, BW, BH, cb_towcable,    false);
    make_btn(screen_buttons, LV_SYMBOL_GPS,      "ARTY STRIKE[M]", X5, Y2, BW, BH, cb_arty_coord,  false);

    // ── BARRE DU BAS ─────────────────────────────────────────────────────────
    lv_obj_t * bot = lv_obj_create(screen_buttons);
    lv_obj_set_size(bot, 800, 38);
    lv_obj_align(bot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bot, lv_color_hex(0x0A1A0A), LV_PART_MAIN);
    lv_obj_set_style_border_color(bot, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_opa(bot, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(bot, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(bot, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * bot_lbl_l = lv_label_create(bot);
    lv_label_set_text(bot_lbl_l, "BRIDGE: START wt_telemetry.py");
    lv_obj_set_style_text_color(bot_lbl_l, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(bot_lbl_l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(bot_lbl_l, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t * bot_l1 = lv_label_create(bot);
    lv_label_set_text(bot_l1, "[L1]");
    lv_obj_set_style_text_color(bot_l1, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(bot_l1, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(bot_l1, LV_ALIGN_CENTER, -55, 0);

    lv_obj_t * bot_l2 = lv_label_create(bot);
    lv_label_set_text(bot_l2, "[L2]");
    lv_obj_set_style_text_color(bot_l2, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(bot_l2, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(bot_l2, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t * bot_l3 = lv_label_create(bot);
    lv_label_set_text(bot_l3, "[L3]");
    lv_obj_set_style_text_color(bot_l3, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(bot_l3, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(bot_l3, LV_ALIGN_CENTER, 55, 0);

    lv_obj_t * bot_lbl_r = lv_label_create(bot);
    lv_label_set_text(bot_lbl_r, "ARMAMENT: HOT");
    lv_obj_set_style_text_color(bot_lbl_r, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(bot_lbl_r, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(bot_lbl_r, LV_ALIGN_RIGHT_MID, -8, 0);
}

// ===== HELPER MINI-BOITE DRIVETRAIN =====
// Cree une boite avec titre, valeur (dynamique) et unite dans un conteneur parent.
static void make_drive_box(lv_obj_t * parent,
                           const char * title_str,
                           lv_obj_t ** val_out,
                           const char * unit_str,
                           int32_t bx, int32_t by) {
    const int32_t BW = 178, BH = 65;
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_set_pos(box, bx, by);
    lv_obj_set_size(box, BW, BH);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x051005), LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 6, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * ttl = lv_label_create(box);
    lv_label_set_text(ttl, title_str);
    lv_obj_set_style_text_color(ttl, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(ttl, 0, 0);

    *val_out = lv_label_create(box);
    lv_label_set_text(*val_out, "--");
    lv_obj_set_style_text_color(*val_out, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(*val_out, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(*val_out, LV_ALIGN_CENTER, 0, 4);

    lv_obj_t * unit = lv_label_create(box);
    lv_label_set_text(unit, unit_str);
    lv_obj_set_style_text_color(unit, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(unit, LV_ALIGN_BOTTOM_MID, 0, 0);
}

// ===== HELPER BARRE DE PROGRESSION ETIQUETEE =====
// Cree une barre de progression avec nom et barre dans un conteneur parent.
static void make_module_bar(lv_obj_t * parent,
                            const char * name_str,
                            int32_t pct,
                            int32_t by) {
    lv_obj_t * name = lv_label_create(parent);
    lv_label_set_text(name, name_str);
    lv_obj_set_style_text_color(name, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(name, 0, by);

    lv_obj_t * bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, 125, by + 1);
    lv_obj_set_size(bar, 200, 12);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0A3302), LV_PART_MAIN);
    lv_obj_set_style_border_color(bar, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 1, LV_PART_MAIN);
    lv_color_t bar_col = (pct > 50) ? COL_NEON : lv_color_hex(0xFF4444);
    lv_obj_set_style_bg_color(bar, bar_col, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);

    char pbuf[8];
    snprintf(pbuf, sizeof(pbuf), "%d%%", pct);
    lv_obj_t * pct_lbl = lv_label_create(parent);
    lv_label_set_text(pct_lbl, pbuf);
    lv_obj_set_style_text_color(pct_lbl, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(pct_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(pct_lbl, 332, by);
}

// ===== ECRAN 2 : VEHICULE STATUS =====
void build_screen_telem() {
    lv_obj_set_style_bg_color(screen_telem, COL_DARK, LV_PART_MAIN);

    // ── EN-TETE ──────────────────────────────────────────────────────────────
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

    // Bouton retour
    lv_obj_t * btn_back_hdr = lv_btn_create(hdr);
    lv_obj_set_pos(btn_back_hdr, 8, 8);
    lv_obj_set_size(btn_back_hdr, 50, 38);
    lv_obj_set_style_bg_color(btn_back_hdr, lv_color_hex(0x001835), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_back_hdr, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_back_hdr, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_back_hdr, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_back_hdr, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_back_hdr, cb_goto_buttons, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_ico = lv_label_create(btn_back_hdr);
    lv_label_set_text(back_ico, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_ico, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(back_ico, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(back_ico);

    // DIAGNOSTICS / statut bridge
    lv_obj_t * diag_ttl = lv_label_create(hdr);
    lv_label_set_text(diag_ttl, "DIAGNOSTICS");
    lv_obj_set_style_text_color(diag_ttl, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(diag_ttl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(diag_ttl, 68, 4);

    telem_status = lv_label_create(hdr);
    lv_label_set_text(telem_status, "BRIDGE OFFLINE");
    lv_obj_set_style_text_color(telem_status, lv_color_hex(0xFF3300), LV_PART_MAIN);
    lv_obj_set_style_text_font(telem_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(telem_status, 68, 25);

    // Titre droit
    lv_obj_t * telem_title = lv_label_create(hdr);
    lv_label_set_text(telem_title, "VEHICLE TELEMETRY");
    lv_obj_set_style_text_color(telem_title, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(telem_title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(telem_title, LV_ALIGN_RIGHT_MID, -10, 0);

    // ── COLONNE GAUCHE (x=10, w=375) ─────────────────────────────────────────

    // Panel CREW STATUS
    lv_obj_t * crew_panel = lv_obj_create(screen_telem);
    lv_obj_set_pos(crew_panel, 10, 65);
    lv_obj_set_size(crew_panel, 375, 173);
    lv_obj_set_style_bg_color(crew_panel, lv_color_hex(0x0A1A0A), LV_PART_MAIN);
    lv_obj_set_style_border_color(crew_panel, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_opa(crew_panel, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(crew_panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(crew_panel, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(crew_panel, 10, LV_PART_MAIN);
    lv_obj_remove_flag(crew_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * crew_ttl = lv_label_create(crew_panel);
    lv_label_set_text(crew_ttl, "CREW STATUS");
    lv_obj_set_style_text_color(crew_ttl, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(crew_ttl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(crew_ttl, 0, 0);

    // Separateur interne
    lv_obj_t * crew_sep = lv_obj_create(crew_panel);
    lv_obj_set_size(crew_sep, 355, 1);
    lv_obj_set_pos(crew_sep, 0, 20);
    lv_obj_set_style_bg_color(crew_sep, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(crew_sep, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(crew_sep, 0, LV_PART_MAIN);
    lv_obj_remove_flag(crew_sep, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * pers_lbl = lv_label_create(crew_panel);
    lv_label_set_text(pers_lbl, "PERSONNEL");
    lv_obj_set_style_text_color(pers_lbl, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(pers_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(pers_lbl, 0, 28);

    telem_crew_count = lv_label_create(crew_panel);
    lv_label_set_text(telem_crew_count, "--/--");
    lv_obj_set_style_text_color(telem_crew_count, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(telem_crew_count, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(telem_crew_count, LV_ALIGN_TOP_RIGHT, 0, 28);

    telem_crew_bar = lv_bar_create(crew_panel);
    lv_obj_set_size(telem_crew_bar, 355, 14);
    lv_obj_set_pos(telem_crew_bar, 0, 52);
    lv_bar_set_range(telem_crew_bar, 0, 100);
    lv_bar_set_value(telem_crew_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(telem_crew_bar, lv_color_hex(0x0A3302), LV_PART_MAIN);
    lv_obj_set_style_border_color(telem_crew_bar, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_border_width(telem_crew_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(telem_crew_bar, COL_NEON, LV_PART_INDICATOR);
    lv_obj_set_style_radius(telem_crew_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(telem_crew_bar, 2, LV_PART_INDICATOR);

    lv_obj_t * crew_pct_lbl = lv_label_create(crew_panel);
    lv_label_set_text(crew_pct_lbl, "0% OPERATIONAL");
    lv_obj_set_style_text_color(crew_pct_lbl, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(crew_pct_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(crew_pct_lbl, LV_ALIGN_TOP_RIGHT, 0, 72);

    lv_obj_t * stab_ttl = lv_label_create(crew_panel);
    lv_label_set_text(stab_ttl, "STABILIZER");
    lv_obj_set_style_text_color(stab_ttl, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(stab_ttl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(stab_ttl, 0, 100);

    lv_obj_t * stab_val = lv_label_create(crew_panel);
    lv_label_set_text(stab_val, "--");
    lv_obj_set_style_text_color(stab_val, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(stab_val, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(stab_val, LV_ALIGN_TOP_RIGHT, 0, 100);

    // Panel AMMUNITION
    lv_obj_t * ammo_panel = lv_obj_create(screen_telem);
    lv_obj_set_pos(ammo_panel, 10, 248);
    lv_obj_set_size(ammo_panel, 375, 168);
    lv_obj_set_style_bg_color(ammo_panel, lv_color_hex(0x0A1A0A), LV_PART_MAIN);
    lv_obj_set_style_border_color(ammo_panel, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_opa(ammo_panel, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(ammo_panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ammo_panel, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ammo_panel, 10, LV_PART_MAIN);
    lv_obj_remove_flag(ammo_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * ammo_ttl = lv_label_create(ammo_panel);
    lv_label_set_text(ammo_ttl, "AMMUNITION");
    lv_obj_set_style_text_color(ammo_ttl, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(ammo_ttl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(ammo_ttl, 0, 0);

    lv_obj_t * ammo_sep = lv_obj_create(ammo_panel);
    lv_obj_set_size(ammo_sep, 355, 1);
    lv_obj_set_pos(ammo_sep, 0, 20);
    lv_obj_set_style_bg_color(ammo_sep, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ammo_sep, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(ammo_sep, 0, LV_PART_MAIN);
    lv_obj_remove_flag(ammo_sep, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * ammo_sub = lv_label_create(ammo_panel);
    lv_label_set_text(ammo_sub, "MAIN ROUND (1ST STAGE)");
    lv_obj_set_style_text_color(ammo_sub, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(ammo_sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(ammo_sub, 0, 28);

    telem_ammo_lbl = lv_label_create(ammo_panel);
    lv_label_set_text(telem_ammo_lbl, "--");
    lv_obj_set_style_text_color(telem_ammo_lbl, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(telem_ammo_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(telem_ammo_lbl, LV_ALIGN_TOP_RIGHT, 0, 28);

    telem_ammo_bar = lv_bar_create(ammo_panel);
    lv_obj_set_size(telem_ammo_bar, 355, 10);
    lv_obj_set_pos(telem_ammo_bar, 0, 52);
    lv_bar_set_range(telem_ammo_bar, 0, 40);
    lv_bar_set_value(telem_ammo_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(telem_ammo_bar, lv_color_hex(0x0A3302), LV_PART_MAIN);
    lv_obj_set_style_border_color(telem_ammo_bar, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_border_width(telem_ammo_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(telem_ammo_bar, COL_NEON, LV_PART_INDICATOR);
    lv_obj_set_style_radius(telem_ammo_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(telem_ammo_bar, 2, LV_PART_INDICATOR);

    lv_obj_t * ammo_note = lv_label_create(ammo_panel);
    lv_label_set_text(ammo_note, "Secondary counters not exposed by WT API.");
    lv_obj_set_style_text_color(ammo_note, lv_color_hex(0x226622), LV_PART_MAIN);
    lv_obj_set_style_text_font(ammo_note, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(ammo_note, 0, 70);

    // ── COLONNE DROITE (x=395, w=395) ────────────────────────────────────────

    // Panel DRIVETRAIN (grille 2x2 : vitesse, rpm, rapport, carburant)
    lv_obj_t * drive_panel = lv_obj_create(screen_telem);
    lv_obj_set_pos(drive_panel, 395, 65);
    lv_obj_set_size(drive_panel, 395, 195);
    lv_obj_set_style_bg_color(drive_panel, lv_color_hex(0x0A1A0A), LV_PART_MAIN);
    lv_obj_set_style_border_color(drive_panel, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_opa(drive_panel, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(drive_panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(drive_panel, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(drive_panel, 10, LV_PART_MAIN);
    lv_obj_remove_flag(drive_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * drive_ttl = lv_label_create(drive_panel);
    lv_label_set_text(drive_ttl, "DRIVETRAIN");
    lv_obj_set_style_text_color(drive_ttl, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(drive_ttl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(drive_ttl, 0, 0);

    lv_obj_t * drive_sep = lv_obj_create(drive_panel);
    lv_obj_set_size(drive_sep, 375, 1);
    lv_obj_set_pos(drive_sep, 0, 20);
    lv_obj_set_style_bg_color(drive_sep, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(drive_sep, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(drive_sep, 0, LV_PART_MAIN);
    lv_obj_remove_flag(drive_sep, LV_OBJ_FLAG_SCROLLABLE);

    // Grille 2x2 des instruments
    make_drive_box(drive_panel, "SPEED", &telem_spd,      "KM/H",    0,   26);
    make_drive_box(drive_panel, "RPM",   &telem_rpm,      "ENGINE",  187, 26);
    make_drive_box(drive_panel, "GEAR",  &telem_gear,     "CURRENT", 0,   97);
    make_drive_box(drive_panel, "FUEL",  &telem_fuel_lbl, "%",       187, 97);

    // Panel MODULE INTEGRITY (valeurs placeholder — API WT ne les expose pas)
    lv_obj_t * mod_panel = lv_obj_create(screen_telem);
    lv_obj_set_pos(mod_panel, 395, 270);
    lv_obj_set_size(mod_panel, 395, 168);
    lv_obj_set_style_bg_color(mod_panel, lv_color_hex(0x0A1A0A), LV_PART_MAIN);
    lv_obj_set_style_border_color(mod_panel, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_opa(mod_panel, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(mod_panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(mod_panel, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(mod_panel, 10, LV_PART_MAIN);
    lv_obj_remove_flag(mod_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * mod_ttl = lv_label_create(mod_panel);
    lv_label_set_text(mod_ttl, "MODULE INTEGRITY");
    lv_obj_set_style_text_color(mod_ttl, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(mod_ttl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(mod_ttl, 0, 0);

    lv_obj_t * mod_sep = lv_obj_create(mod_panel);
    lv_obj_set_size(mod_sep, 375, 1);
    lv_obj_set_pos(mod_sep, 0, 20);
    lv_obj_set_style_bg_color(mod_sep, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(mod_sep, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(mod_sep, 0, LV_PART_MAIN);
    lv_obj_remove_flag(mod_sep, LV_OBJ_FLAG_SCROLLABLE);

    // Barres de modules (placeholder — WT API n'expose pas les degats de modules)
    static const char * mod_names[] = { "ENGINE", "TRANSMISSION", "TURRET DRV", "GUN BARREL", "TRACK L", "TRACK R" };
    static const int    mod_pct[]   = { 100, 100, 100, 100, 85, 90 };
    for (int i = 0; i < 6; i++) {
        make_module_bar(mod_panel, mod_names[i], mod_pct[i], 26 + i * 22);
    }

    // ── BARRE DU BAS ─────────────────────────────────────────────────────────
    lv_obj_t * bot = lv_obj_create(screen_telem);
    lv_obj_set_size(bot, 800, 38);
    lv_obj_align(bot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bot, lv_color_hex(0x0A1A0A), LV_PART_MAIN);
    lv_obj_set_style_border_color(bot, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_opa(bot, LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_border_width(bot, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(bot, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * bot_l = lv_label_create(bot);
    lv_label_set_text(bot_l, "TELEMETRY: BRIDGE OFFLINE");
    lv_obj_set_style_text_color(bot_l, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(bot_l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(bot_l, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t * bot_r = lv_label_create(bot);
    lv_label_set_text(bot_r, "STAB: --");
    lv_obj_set_style_text_color(bot_r, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(bot_r, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(bot_r, LV_ALIGN_RIGHT_MID, -8, 0);
}

// ===== ECRAN 3 : CARTE TACTIQUE =====
void build_screen_map() {
    lv_obj_set_style_bg_color(screen_map, COL_DARK, LV_PART_MAIN);

    // ── EN-TETE ──────────────────────────────────────────────────────────────
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

    // Bouton retour
    lv_obj_t * btn_back_hdr = lv_btn_create(hdr);
    lv_obj_set_pos(btn_back_hdr, 8, 8);
    lv_obj_set_size(btn_back_hdr, 50, 38);
    lv_obj_set_style_bg_color(btn_back_hdr, lv_color_hex(0x001835), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_back_hdr, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_back_hdr, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_back_hdr, 4, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn_back_hdr, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_back_hdr, cb_goto_buttons, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_ico = lv_label_create(btn_back_hdr);
    lv_label_set_text(back_ico, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_ico, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(back_ico, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(back_ico);

    // SAT-LINK / statut
    lv_obj_t * sat_ttl = lv_label_create(hdr);
    lv_label_set_text(sat_ttl, "SAT-LINK");
    lv_obj_set_style_text_color(sat_ttl, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(sat_ttl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(sat_ttl, 68, 4);

    lv_obj_t * sat_val = lv_label_create(hdr);
    lv_label_set_text(sat_val, "BRIDGE OFFLINE");
    lv_obj_set_style_text_color(sat_val, lv_color_hex(0xFF3300), LV_PART_MAIN);
    lv_obj_set_style_text_font(sat_val, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(sat_val, 68, 25);

    // Titre droit : TACTICAL GRID // nom de carte
    lv_obj_t * grid_ttl = lv_label_create(hdr);
    lv_label_set_text(grid_ttl, "TACTICAL GRID // ");
    lv_obj_set_style_text_color(grid_ttl, lv_color_hex(0x336633), LV_PART_MAIN);
    lv_obj_set_style_text_font(grid_ttl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(grid_ttl, LV_ALIGN_RIGHT_MID, -90, 0);

    map_name_label = lv_label_create(hdr);
    lv_label_set_text(map_name_label, "---");
    lv_obj_set_style_text_color(map_name_label, COL_NEON, LV_PART_MAIN);
    lv_obj_set_style_text_font(map_name_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(map_name_label, LV_ALIGN_RIGHT_MID, -8, 0);

    // ── CONTENEUR CARTE ──────────────────────────────────────────────────────
    // Map display container — dots are positioned inside relative to (0,0) top-left.
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

    // ── Image d'arriere-plan (z-order le plus bas) ────────────────────────────
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

    // ── Anneaux radar (style Figma — dessus de l'image, dessous des points) ──
    // Centre du conteneur : (MAP_CONT_W/2, MAP_CONT_H/2) = (370, 162)
    // Rayons : 50, 100, 150 px (contrainte verticale : r <= 162)
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

    // Waiting placeholder shown until the first map message arrives.
    map_wait_label = lv_label_create(map_container);
    lv_label_set_text(map_wait_label, "En attente de donnees carte...");
    lv_obj_set_style_text_color(map_wait_label, lv_color_hex(0x334433), LV_PART_MAIN);
    lv_obj_set_style_text_font(map_wait_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(map_wait_label);

    // Pre-allocate dot pool (all hidden at startup, shown/repositioned on map update).
    for (int i = 0; i < MAP_MAX_ENT; i++) {
        map_dots[i] = lv_obj_create(map_container);
        lv_obj_set_size(map_dots[i], MAP_ENT_SIZE, MAP_ENT_SIZE);
        lv_obj_set_style_radius(map_dots[i], LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_border_width(map_dots[i], 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(map_dots[i], 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(map_dots[i], lv_color_hex(0x888888), LV_PART_MAIN);
        lv_obj_add_flag(map_dots[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(map_dots[i], 0, 0);
    }

    // ── Legende (style Figma avec points colores) ─────────────────────────────
    // Couleurs correspondant a color_for_type()
    lv_color_t leg_cols[4];
    const char * leg_txts[4] = { "ALLY", "ENEMY", "OBJECTIVE", "AIRFIELD" };
    leg_cols[0] = lv_color_hex(0x00CC00);  // vert  - allie
    leg_cols[1] = lv_color_hex(0xFF2200);  // rouge - ennemi
    leg_cols[2] = lv_color_hex(0xFFCC00);  // jaune - objectif
    leg_cols[3] = lv_color_hex(0x555555);  // gris  - aerodrome
    int32_t lx = 32;
    const int32_t LEG_Y = MAP_CONT_H + 64 + 8;  // juste sous le conteneur
    for (int i = 0; i < 4; i++) {
        lv_obj_t * dot = lv_obj_create(screen_map);
        lv_obj_set_pos(dot, lx, LEG_Y + 2);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_bg_color(dot, leg_cols[i], LV_PART_MAIN);
        lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lx += 14;

        lv_obj_t * txt = lv_label_create(screen_map);
        lv_label_set_text(txt, leg_txts[i]);
        lv_obj_set_style_text_color(txt, leg_cols[i], LV_PART_MAIN);
        lv_obj_set_style_text_font(txt, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_pos(txt, lx, LEG_Y);
        // Estime la largeur du texte (~8 px/char a font_14) + marge
        lx += (int32_t)(strlen(leg_txts[i]) * 8 + 16);
    }
}

// ===== PARSING SERIAL - TELEMETRIE (serial thread, no LVGL) =====
// Fills *out* from a raw telemetry line.  Must NOT call any lv_* function.
// Expected format: SPD:{int}|RPM:{int}|GEAR:{val}|AMMO:{int}|STAB:{0/1}|FUEL:{int}|...|STATUS:{0/1}
static void parse_telem_string(const String& data, TelemShared& out) {
    // Store raw line for debug
    data.toCharArray(out.raw, sizeof(out.raw) - 1);
    out.raw[sizeof(out.raw) - 1] = '\0';

    int idx;

    idx = data.indexOf("SPD:");
    out.spd = (idx >= 0) ? data.substring(idx + 4, data.indexOf("|", idx)).toInt() : 0;

    idx = data.indexOf("RPM:");
    out.rpm = (idx >= 0) ? data.substring(idx + 4, data.indexOf("|", idx)).toInt() : 0;

    idx = data.indexOf("GEAR:");
    if (idx >= 0) {
        String g = data.substring(idx + 5, data.indexOf("|", idx));
        g.toCharArray(out.gear, sizeof(out.gear));
    } else {
        snprintf(out.gear, sizeof(out.gear), "---");
    }

    idx = data.indexOf("AMMO:");
    out.ammo = (idx >= 0) ? data.substring(idx + 5, data.indexOf("|", idx)).toInt() : -1;

    idx = data.indexOf("STAB:");
    out.stab = (idx >= 0) ? data.substring(idx + 5, data.indexOf("|", idx)).toInt() : 0;

    idx = data.indexOf("FUEL:");
    out.fuel = (idx >= 0) ? data.substring(idx + 5, data.indexOf("|", idx)).toInt() : -1;

    idx = data.indexOf("TANK:");
    if (idx >= 0) {
        String t = data.substring(idx + 5, data.indexOf("|", idx));
        t.toCharArray(out.tank, sizeof(out.tank));
    } else {
        snprintf(out.tank, sizeof(out.tank), "UNKNOWN");
    }

    out.crew = 0; out.crew_total = 0;
    idx = data.indexOf("CREW:");
    if (idx >= 0) {
        String c = data.substring(idx + 5, data.indexOf("|", idx));
        int sl = c.indexOf('/');
        if (sl >= 0) {
            out.crew       = c.substring(0, sl).toInt();
            out.crew_total = c.substring(sl + 1).toInt();
        }
    }

    idx = data.indexOf("STATUS:");
    if (idx >= 0) {
        String s = data.substring(idx + 7);
        s.trim();
        out.online = s.startsWith("1");
    } else {
        out.online = false;
    }
}

// ===== PARSING SERIAL - CARTE (serial thread, no LVGL) =====
// Fills *out* from a MAPNAME/MAPOBJ line.  Must NOT call any lv_* function.
// Expected format: MAPNAME:{name}|MAPOBJ:{x},{y},{type};...
// Type codes: A=allié, E=ennemi, O=objectif, F=aerodrome, N=autre.
static void parse_map_string(const String& data, MapShared& out) {
    out.count    = 0;
    out.name[0]  = '\0';

    int idx_name = data.indexOf("MAPNAME:");
    if (idx_name >= 0) {
        int ns = idx_name + 8;
        int ne = data.indexOf("|", ns);
        String mn = (ne >= 0) ? data.substring(ns, ne) : data.substring(ns);
        mn.toCharArray(out.name, sizeof(out.name));
    }

    int idx_obj = data.indexOf("MAPOBJ:");
    if (idx_obj < 0) return;
    String obj_str = data.substring(idx_obj + 7);
    if (obj_str == "-" || obj_str.length() == 0) return;

    int start = 0;
    while (out.count < MAP_MAX_ENT) {
        int sep    = obj_str.indexOf(';', start);
        String ent = (sep >= 0) ? obj_str.substring(start, sep)
                                : obj_str.substring(start);
        int c1 = ent.indexOf(',');
        int c2 = (c1 >= 0) ? ent.indexOf(',', c1 + 1) : -1;
        if (c1 < 0 || c2 < 0 || c2 + 1 >= (int)ent.length()) {
            if (sep < 0) break;
            start = sep + 1;
            continue;
        }
        out.ents[out.count].x    = ent.substring(0, c1).toFloat();
        out.ents[out.count].y    = ent.substring(c1 + 1, c2).toFloat();
        out.ents[out.count].type = ent.charAt(c2 + 1);
        out.count++;
        if (sep < 0) break;
        start = sep + 1;
    }
}

// ===== LVGL UPDATE - TELEMETRIE (main/M7 thread only) =====
// Reads a local copy of TelemShared (no mutex held) and updates LVGL widgets.
static void apply_telem_update(const TelemShared& d) {
    char buf[96];

    // ── HUD ligne principale (ecran boutons) ─────────────────────────────────
    snprintf(buf, sizeof(buf), "%.32s | %d km/h | CREW:%d/%d",
             d.tank, d.spd, d.crew, d.crew_total);
    lv_label_set_text(hud_label_btn, buf);

    // Statut bridge (ecran boutons)
    if (hud_bridge_lbl) {
        if (d.online) {
            lv_label_set_text(hud_bridge_lbl, "ACTIVE");
            lv_obj_set_style_text_color(hud_bridge_lbl, COL_NEON, LV_PART_MAIN);
        } else {
            lv_label_set_text(hud_bridge_lbl, "OFFLINE");
            lv_obj_set_style_text_color(hud_bridge_lbl, lv_color_hex(0xFF3300), LV_PART_MAIN);
        }
    }

    // Statut bridge (ecran vehicule status)
    if (d.online) {
        lv_label_set_text(telem_status, "LIVE");
        lv_obj_set_style_text_color(telem_status, COL_NEON, LV_PART_MAIN);
    } else {
        lv_label_set_text(telem_status, "BRIDGE OFFLINE");
        lv_obj_set_style_text_color(telem_status, lv_color_hex(0xFF3300), LV_PART_MAIN);
    }

    // ── Instruments drivetrain ────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "%d", d.spd);
    lv_label_set_text(telem_spd, buf);

    snprintf(buf, sizeof(buf), "%d", d.rpm);
    lv_label_set_text(telem_rpm, buf);

    lv_label_set_text(telem_gear, d.gear);

    if (telem_fuel_lbl) {
        if (d.fuel >= 0) {
            snprintf(buf, sizeof(buf), "%d", d.fuel);
            lv_label_set_text(telem_fuel_lbl, buf);
            lv_obj_set_style_text_color(telem_fuel_lbl,
                d.fuel < 20 ? lv_color_hex(0xFF4444) : COL_NEON, LV_PART_MAIN);
        } else {
            lv_label_set_text(telem_fuel_lbl, "N/A");
        }
    }

    // ── Crew bar ──────────────────────────────────────────────────────────────
    if (telem_crew_count) {
        snprintf(buf, sizeof(buf), "%d/%d", d.crew, d.crew_total);
        lv_label_set_text(telem_crew_count, buf);
        lv_color_t cnt_col = (d.crew_total > 0 && d.crew < d.crew_total)
                             ? lv_color_hex(0xFF4444) : COL_NEON;
        lv_obj_set_style_text_color(telem_crew_count, cnt_col, LV_PART_MAIN);
    }
    if (telem_crew_bar) {
        int pct = (d.crew_total > 0) ? (d.crew * 100 / d.crew_total) : 0;
        lv_bar_set_value(telem_crew_bar, pct, LV_ANIM_OFF);
        lv_color_t bar_col = (pct > 50) ? COL_NEON
                           : (pct > 25) ? lv_color_hex(0xFFCC00)
                                        : lv_color_hex(0xFF4444);
        lv_obj_set_style_bg_color(telem_crew_bar, bar_col, LV_PART_INDICATOR);
    }

    // ── Ammunition bar ────────────────────────────────────────────────────────
    if (telem_ammo_lbl) {
        if (d.ammo >= 0) {
            snprintf(buf, sizeof(buf), "%d", d.ammo);
            lv_label_set_text(telem_ammo_lbl, buf);
            lv_color_t a_col = (d.ammo < 5) ? lv_color_hex(0xFF4444) : COL_NEON;
            lv_obj_set_style_text_color(telem_ammo_lbl, a_col, LV_PART_MAIN);
        } else {
            lv_label_set_text(telem_ammo_lbl, "--");
        }
    }
    if (telem_ammo_bar && d.ammo >= 0) {
        lv_bar_set_value(telem_ammo_bar, d.ammo, LV_ANIM_OFF);
        lv_color_t a_col = (d.ammo > 10) ? COL_NEON
                         : (d.ammo > 5)  ? lv_color_hex(0xFFCC00)
                                         : lv_color_hex(0xFF4444);
        lv_obj_set_style_bg_color(telem_ammo_bar, a_col, LV_PART_INDICATOR);
    }
}

// ===== LVGL UPDATE - CARTE (main/M7 thread only) =====
// Reads a local copy of MapShared (no mutex held) and repositions map dots.
static void apply_map_update(const MapShared& d) {
    lv_label_set_text(map_name_label, d.name);

    // Hide all dots first
    for (int i = 0; i < MAP_MAX_ENT; i++) {
        lv_obj_add_flag(map_dots[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (d.count == 0) return;

    // Hide the "waiting" placeholder once real data has arrived
    lv_obj_add_flag(map_wait_label, LV_OBJ_FLAG_HIDDEN);

    const int32_t max_px = MAP_CONT_W - MAP_ENT_SIZE;
    const int32_t max_py = MAP_CONT_H - MAP_ENT_SIZE;

    for (int i = 0; i < d.count && i < MAP_MAX_ENT; i++) {
        int32_t px = (int32_t)(d.ents[i].x * max_px);
        int32_t py = (int32_t)(d.ents[i].y * max_py);
        if (px < 0) px = 0; else if (px > max_px) px = max_px;
        if (py < 0) py = 0; else if (py > max_py) py = max_py;
        lv_obj_set_pos(map_dots[i], px, py);
        lv_obj_set_style_bg_color(map_dots[i], color_for_type(d.ents[i].type), LV_PART_MAIN);
        lv_obj_remove_flag(map_dots[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// ===== LVGL UPDATE - IMAGE D'ARRIERE-PLAN (thread M7 uniquement) =====
// Appele apres qu'un nouveau MAPRAW: a ete decode dans g_map_raw.
// g_data_mutex n'est PAS tenu ici : les messages MAPRAW sont espaces d'au moins plusieurs
// secondes (un par partie), donc le tampon est stable le temps du rendu.
static void apply_map_image() {
    if (map_bg_img == NULL) return;
    // Forcer LVGL a relire le descripteur et invalider la zone d'affichage.
    lv_image_set_src(map_bg_img, &map_img_dsc);
    lv_obj_remove_flag(map_bg_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(map_bg_img);
}

// ===== THREAD SERIE (role M4 : donnees & reseau) =====
// Runs at osPriorityHigh. Reads bytes from Serial, assembles lines, parses
// them into the shared structs, and signals the main loop via the update
// flags. LVGL is never touched here.
//
// Deux modes de reception :
//   - Mode texte   : accumule les caracteres dans text_buf (max 512) jusqu'au '\n'.
//                    Detecte le prefixe "MAPRAW:" (7 chars) et bascule en mode image.
//   - Mode image   : accumule le payload base64 dans s_b64buf (26 KB statique) jusqu'au '\n',
//                    puis decode en RGB565 dans g_map_raw et leve g_img_updated.
void serial_task() {
    String text_buf;
    text_buf.reserve(512);

    // Tampon statique dedie au payload base64 des messages MAPRAW:
    // Alloue en BSS (static) pour ne pas saturer les 8 KB de stack du thread.
    static uint8_t s_b64buf[MAP_B64_MAX];
    static size_t  s_b64len   = 0;
    static bool    s_in_image = false;   // true = on accumule un payload MAPRAW:

    while (true) {
        while (Serial.available()) {
            char c = (char)Serial.read();

            if (c == '\n') {
                if (s_in_image) {
                    // Payload base64 complet → decode dans le tampon partage
                    uint32_t decoded = b64_decode_buf(s_b64buf, s_b64len,
                                                      g_map_raw, MAP_RAW_BYTES);
                    if (decoded == MAP_RAW_BYTES) {
                        g_data_mutex.lock();
                        g_img_updated = true;
                        g_data_mutex.unlock();
                    }
                    s_in_image = false;
                    s_b64len   = 0;
                } else {
                    text_buf.trim();
                    if (text_buf.length() > 0) {
                        if (text_buf.startsWith("MAPNAME:")) {
                            // Parse outside the mutex to keep lock time minimal
                            MapShared local;
                            parse_map_string(text_buf, local);
                            g_data_mutex.lock();
                            g_map         = local;
                            g_map_updated = true;
                            g_data_mutex.unlock();
                        } else {
                            TelemShared local;
                            parse_telem_string(text_buf, local);
                            g_data_mutex.lock();
                            g_telem         = local;
                            g_telem_updated = true;
                            g_data_mutex.unlock();
                        }
                    }
                    text_buf = "";
                }
            } else {
                if (s_in_image) {
                    // Accumule le payload base64
                    if (s_b64len < MAP_B64_MAX - 1)
                        s_b64buf[s_b64len++] = (uint8_t)c;
                    // else : trame corrompue / trop grande — octet ignore silencieusement
                } else {
                    if ((size_t)text_buf.length() < 512) {
                        text_buf += c;
                        // Apres 7 caracteres, verifie si c'est un prefixe MAPRAW:
                        if (text_buf.length() == 7 && text_buf.equals("MAPRAW:")) {
                            s_in_image = true;
                            s_b64len   = 0;
                            text_buf   = "";  // Efface le prefixe (dimensions connues a la compile)
                        }
                    } else {
                        text_buf = "";  // Drop runaway frame
                    }
                }
            }
        }
        // Yield for 2 ms so the M7 loop and USB stack get CPU time
        delay(2);
    }
}

// ===== RPC RECEIVE BUFFER (M4 → M7 parsed frames) =====
static String rpcRecvBuffer = "";
static const size_t RPC_RECV_BUF_MAX = 512;

// ===== SETUP =====
void setup() {
    Serial.begin(115200);
    // Initialise le canal inter-cœurs OpenAMP/RPMsg.
    // DOIT être appelé avant tout autre begin() afin que le M4 puisse
    // terminer son propre RPC.begin() sans se bloquer indéfiniment.
    RPC.begin();
    // No setTimeout needed — the serial thread uses non-blocking reads
    Display.begin();
    TouchDetector.begin();

    // Palette Figma : vert neon sur fond tres sombre
    COL_NEON   = lv_color_hex(0x39FF14);  // vert neon #39FF14
    COL_DANGER = lv_color_hex(0xEF4444);  // rouge alerte
    COL_ARMOR  = lv_color_hex(0x4A5D23);  // olive (conserve)
    COL_TECH   = lv_color_hex(0x2F4F4F);  // bleu-gris (conserve)
    COL_DARK   = lv_color_hex(0x051005);  // fond tres sombre #051005
    COL_BAR    = lv_color_hex(0x0A1A0A);  // fond en-tete

    screen_buttons = lv_obj_create(NULL);
    screen_telem   = lv_obj_create(NULL);
    screen_map     = lv_obj_create(NULL);

    build_screen_buttons();
    build_screen_telem();
    build_screen_map();

    // LVGL 9: lv_scr_load -> lv_screen_load
    lv_screen_load(screen_buttons);

    // Start the serial-reader thread (M4-equivalent role).
    // It runs independently, never touching LVGL.
    g_serial_thread.start(serial_task);
}

// ===== LOOP (role M7 : rendu & tactile) =====
// The loop exclusively drives LVGL and USB-HID.
// It reads pre-parsed data from the shared structs under a brief mutex lock,
// then applies any pending updates to LVGL widgets — all without blocking on
// serial I/O. delay(5) yields CPU to the USB stack, which is why HID button
// presses are reliably delivered to the PC.
void loop() {
    // ── Snapshot shared data under mutex (critical section is a struct copy only) ──
    static TelemShared local_telem;
    static MapShared   local_map;
    bool do_telem = false, do_map = false, do_img = false;

    g_data_mutex.lock();
    if (g_telem_updated) {
        local_telem     = g_telem;
        do_telem        = true;
        g_telem_updated = false;
    }
    if (g_map_updated) {
        local_map    = g_map;
        do_map       = true;
        g_map_updated = false;
    }
    if (g_img_updated) {
        // g_map_raw est deja le tampon cible : pas de copie necessaire.
        do_img        = true;
        g_img_updated = false;
    }
    g_data_mutex.unlock();

    // ── Apply pending LVGL updates (no mutex held) ──
    if (do_telem) apply_telem_update(local_telem);
    if (do_map)   apply_map_update(local_map);
    if (do_img)   apply_map_image();

    // ── Horloge (temps ecoule depuis le demarrage) ───────────────────────────
    // Mise a jour toutes les secondes ; n'utilise pas de RTC, donc affiche le
    // temps ecoule depuis la mise sous tension, pas l'heure reelle.
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

    // ── Drive LVGL rendering and touch events ──
    lv_timer_handler();

    // ── Deferred HID dispatch (after LVGL, before USB yield) ──
    // Sending here — outside lv_timer_handler() — guarantees the USB stack
    // gets a full 5 ms flush window for every key press.
    {
        char hid = g_pending_hid;
        if (hid != '\0') {
            g_pending_hid = '\0';
            if (hid == '\x01') Keyboard.key_code(KEY_SHIFT);
            else               Keyboard.printf("%c", hid);
        }
    }

    // ── Yield to USB stack and other Mbed RTOS tasks ──
    // 5 ms gives the USB HID endpoint time to flush between button presses,
    // eliminating the "1-in-20" missed-keypress issue.
    delay(5);
}
