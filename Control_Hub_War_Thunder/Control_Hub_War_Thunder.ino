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
lv_obj_t * telem_ammo;
lv_obj_t * telem_crew_val;
lv_obj_t * telem_raw;
lv_obj_t * telem_status;
lv_obj_t * telem_cpu_m7;
lv_obj_t * telem_cpu_m4;

// ===== ALERTE AMMO (timer de clignotement) =====
static lv_timer_t * g_ammo_blink_timer = NULL;
static bool         g_ammo_blink_on    = false;

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
// 296 × 2.5 = 740 = MAP_CONT_W, 130 × 2.5 = 325 = MAP_CONT_H → echelle 2.5× exacte.
#define MAP_RAW_W      296
#define MAP_RAW_H      130
// Facteur d'echelle LVGL : 256 = 100%, donc 2.5× = 640.
#define MAP_BG_SCALE   640
// Taille du tampon RGB565 en octets (2 octets par pixel).
#define MAP_RAW_BYTES  (MAP_RAW_W * MAP_RAW_H * 2)
// Taille max du payload base64 (ceil(MAP_RAW_BYTES × 4 / 3) + marge de securite).
#define MAP_B64_MAX    110000

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
    int  ammo;
    int  stab;
    int  crew;
    int  crew_total;
    char tank[48];
    bool online;
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

// ===== CHARGE CPU (M7 + M4) =====
// g_m7_load_pct est mis a jour par le thread d'inactivite (ci-dessous).
// g_m4_load_pct est recu depuis le M4 via RPC.
static volatile uint32_t g_m7_load_pct = 0;
static volatile uint32_t g_m4_load_pct = 0;
// Thread de mesure CPU a priorite basse : compte ses iterations par seconde.
// Il tourne lorsque les threads Normal/High sont en attente (delay, mutex, I/O).
// Stack 2048 octets : le Cortex-M7 avec FPU utilise ~136 octets par frame d'exception ;
// avec plusieurs IRQ imbriquees, 512 octets etait insuffisant et provoquait un HardFault.
// osPriorityIdle est reserve au thread idle interne de Mbed OS ; on utilise
// osPriorityLow pour eviter tout conflit avec ce thread systeme.
static rtos::Thread g_cpu_idle_thread(osPriorityLow, 2048);

// ===== COULEURS =====
lv_color_t COL_DANGER;
lv_color_t COL_ARMOR;
lv_color_t COL_TECH;
lv_color_t COL_DARK;
lv_color_t COL_BAR;

// ===== CALLBACKS HID =====
// Les callbacks positionnent un drapeau ; la boucle principale envoie le rapport HID
// apres lv_timer_handler(), ce qui laisse ensuite delay(5) au stack USB pour transmettre
// la trame — correction du probleme "1 appui sur 20 ignore".
static volatile char    g_hid_char    = '\0';  // '\0' = aucune action en attente
static volatile uint8_t g_hid_keycode = 0;     //  0   = aucun keycode en attente

static void cb_extincteur(lv_event_t * e)  { g_hid_char = '6'; }
static void cb_fumigene(lv_event_t * e)    { g_hid_char = 'g'; }
static void cb_artillerie(lv_event_t * e)  { g_hid_char = '5'; }
static void cb_jumelles(lv_event_t * e)    { g_hid_char = 'b'; }
static void cb_sniper(lv_event_t * e)      { g_hid_keycode = KEY_SHIFT; }
static void cb_moteur(lv_event_t * e)      { g_hid_char = 'i'; }
static void cb_reparation(lv_event_t * e)  { g_hid_char = 'f'; }

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

// ===== CREATION BOUTON =====
// LVGL 9: lv_coord_t removed, use int32_t
void make_btn(lv_obj_t * parent, const char* icon, const char* label,
              int32_t x, int32_t y, int32_t w, int32_t h,
              lv_event_cb_t hid_cb, lv_color_t * color) {

    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, *color, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x111111), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 18, LV_PART_MAIN);
    lv_obj_set_style_shadow_color(btn, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_add_event_cb(btn, hid_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn, btn_visual_cb, LV_EVENT_PRESSED,    color);
    lv_obj_add_event_cb(btn, btn_visual_cb, LV_EVENT_RELEASED,   color);
    lv_obj_add_event_cb(btn, btn_visual_cb, LV_EVENT_PRESS_LOST, color);

    lv_obj_t * ico = lv_label_create(btn);
    lv_label_set_text(ico, icon);
    lv_obj_set_style_text_font(ico, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ico, LV_ALIGN_CENTER, 0, -18);

    lv_obj_t * lbl = lv_label_create(btn);
    lv_label_set_text(lbl, label);
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

// ===== ECRAN 1 : COMMANDES =====
void build_screen_buttons() {
    lv_obj_set_style_bg_color(screen_buttons, COL_DARK, LV_PART_MAIN);
    make_hud_bar(screen_buttons, &hud_label_btn);

    // TELEMETRY button (repositioned to leave room for the new CARTE button)
    lv_obj_t * btn_telem = lv_btn_create(screen_buttons);
    lv_obj_set_pos(btn_telem, 488, 6);
    lv_obj_set_size(btn_telem, 147, 38);
    lv_obj_set_style_bg_color(btn_telem, lv_color_hex(0x00552A), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_telem, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_telem, cb_goto_telem, LV_EVENT_CLICKED, NULL);
    lv_obj_t * telem_lbl = lv_label_create(btn_telem);
    lv_label_set_text(telem_lbl, LV_SYMBOL_RIGHT " TELEMETRY");
    lv_obj_set_style_text_font(telem_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(telem_lbl);

    // CARTE button (new — leads to Screen 3)
    lv_obj_t * btn_carte = lv_btn_create(screen_buttons);
    lv_obj_set_pos(btn_carte, 643, 6);
    lv_obj_set_size(btn_carte, 150, 38);
    lv_obj_set_style_bg_color(btn_carte, lv_color_hex(0x004466), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_carte, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_carte, cb_goto_map, LV_EVENT_CLICKED, NULL);
    lv_obj_t * carte_lbl = lv_label_create(btn_carte);
    lv_label_set_text(carte_lbl, LV_SYMBOL_GPS " CARTE");
    lv_obj_set_style_text_font(carte_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(carte_lbl);

    int32_t W = 228, H = 178;
    int32_t Y1 = 60, Y2 = 255;
    int32_t X1 = 12, X2 = 252, X3 = 492;

    make_btn(screen_buttons, LV_SYMBOL_WARNING,  "EXTINCTEUR [6]", X1, Y1, W, H, cb_extincteur, &COL_DANGER);
    make_btn(screen_buttons, LV_SYMBOL_WIFI,     "FUMIGENE [G]",   X2, Y1, W, H, cb_fumigene,   &COL_ARMOR);
    make_btn(screen_buttons, LV_SYMBOL_GPS,      "ARTILLERIE [5]", X3, Y1, W, H, cb_artillerie, &COL_ARMOR);
    make_btn(screen_buttons, LV_SYMBOL_IMAGE,    "JUMELLES [B]",   X1, Y2, W, H, cb_jumelles,   &COL_TECH);
    make_btn(screen_buttons, LV_SYMBOL_VIDEO,    "VUE TIREUR",     X2, Y2, W, H, cb_sniper,     &COL_TECH);
    make_btn(screen_buttons, LV_SYMBOL_SETTINGS, "MOTEUR [I]",     X3, Y2, W, H, cb_moteur,     &COL_ARMOR);

    lv_obj_t * btn_rep = lv_btn_create(screen_buttons);
    lv_obj_set_pos(btn_rep, 732, 60);
    lv_obj_set_size(btn_rep, 58, 373);
    lv_obj_set_style_bg_color(btn_rep, lv_color_hex(0x2a5500), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_rep, 10, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_rep, cb_reparation, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(btn_rep, btn_visual_cb, LV_EVENT_PRESSED,    &COL_ARMOR);
    lv_obj_add_event_cb(btn_rep, btn_visual_cb, LV_EVENT_RELEASED,   &COL_ARMOR);
    lv_obj_add_event_cb(btn_rep, btn_visual_cb, LV_EVENT_PRESS_LOST, &COL_ARMOR);
    lv_obj_t * rep_lbl = lv_label_create(btn_rep);
    lv_label_set_text(rep_lbl, "REP\nARA\nTION\n[F]");
    lv_obj_set_style_text_font(rep_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(rep_lbl);
}

// ===== HELPER BLOC TELEMETRIE =====
// LVGL 9: lv_coord_t removed, use int32_t
void make_data_block(lv_obj_t * parent, const char* title_str, lv_obj_t ** val_label,
                     int32_t x, int32_t y) {
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_set_size(box, 220, 110);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1C2C1C), LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 8, LV_PART_MAIN);
    // LVGL 9: lv_obj_clear_flag -> lv_obj_remove_flag
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * ttl = lv_label_create(box);
    lv_label_set_text(ttl, title_str);
    lv_obj_set_style_text_color(ttl, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 8);

    *val_label = lv_label_create(box);
    lv_label_set_text(*val_label, "---");
    lv_obj_set_style_text_color(*val_label, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(*val_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(*val_label, LV_ALIGN_CENTER, 0, 12);
}

// ===== ECRAN 2 : TELEMETRIE =====
void build_screen_telem() {
    lv_obj_set_style_bg_color(screen_telem, COL_DARK, LV_PART_MAIN);

    lv_obj_t * title = lv_label_create(screen_telem);
    lv_label_set_text(title, "[ VEHICLE TELEMETRY - WAR THUNDER LINK ]");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    lv_obj_t * sep = lv_obj_create(screen_telem);
    lv_obj_set_size(sep, 780, 2);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, 42);

    telem_status = lv_label_create(screen_telem);
    lv_label_set_text(telem_status, "[ ] PC BRIDGE: OFFLINE");
    lv_obj_set_style_text_color(telem_status, lv_color_hex(0xFF3300), LV_PART_MAIN);
    lv_obj_set_style_text_font(telem_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(telem_status, LV_ALIGN_TOP_LEFT, 30, 55);

    // Charge CPU M7 (haut gauche) et M4 (haut droite)
    telem_cpu_m7 = lv_label_create(screen_telem);
    lv_label_set_text(telem_cpu_m7, "M7: --%");
    lv_obj_set_style_text_color(telem_cpu_m7, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(telem_cpu_m7, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(telem_cpu_m7, LV_ALIGN_TOP_LEFT, 30, 78);

    telem_cpu_m4 = lv_label_create(screen_telem);
    lv_label_set_text(telem_cpu_m4, "M4: --%");
    lv_obj_set_style_text_color(telem_cpu_m4, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(telem_cpu_m4, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(telem_cpu_m4, LV_ALIGN_TOP_RIGHT, -30, 78);

    make_data_block(screen_telem, "VITESSE (km/h)", &telem_spd,      30,  110);
    make_data_block(screen_telem, "OBUS RESTANTS",  &telem_ammo,     290, 110);
    make_data_block(screen_telem, "EQUIPAGE",       &telem_crew_val, 550, 110);

    lv_obj_t * raw_title = lv_label_create(screen_telem);
    lv_label_set_text(raw_title, "RAW DATA STREAM:");
    lv_obj_set_style_text_color(raw_title, lv_color_hex(0x666666), LV_PART_MAIN);
    lv_obj_set_style_text_font(raw_title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(raw_title, LV_ALIGN_TOP_LEFT, 30, 240);

    telem_raw = lv_label_create(screen_telem);
    lv_label_set_text(telem_raw, "En attente de donnees...");
    lv_label_set_long_mode(telem_raw, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(telem_raw, 740);
    lv_obj_set_style_text_color(telem_raw, lv_color_hex(0x00CC00), LV_PART_MAIN);
    lv_obj_set_style_text_font(telem_raw, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(telem_raw, LV_ALIGN_TOP_LEFT, 30, 262);

    lv_obj_t * btn_back = lv_btn_create(screen_telem);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_set_size(btn_back, 300, 50);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x552200), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_back, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_back, cb_goto_buttons, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_lbl = lv_label_create(btn_back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " PANNEAU DE COMMANDES");
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(back_lbl);
}

// ===== ECRAN 3 : CARTE TACTIQUE =====
void build_screen_map() {
    lv_obj_set_style_bg_color(screen_map, COL_DARK, LV_PART_MAIN);

    lv_obj_t * title = lv_label_create(screen_map);
    lv_label_set_text(title, "[ CARTE TACTIQUE - WAR THUNDER ]");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 30, 10);

    map_name_label = lv_label_create(screen_map);
    lv_label_set_text(map_name_label, "---");
    lv_obj_set_style_text_color(map_name_label, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(map_name_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(map_name_label, LV_ALIGN_TOP_RIGHT, -30, 10);

    lv_obj_t * sep = lv_obj_create(screen_map);
    lv_obj_set_size(sep, 780, 2);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, 35);

    // Map display container — dots are positioned inside relative to (0,0) top-left.
    map_container = lv_obj_create(screen_map);
    lv_obj_set_size(map_container, MAP_CONT_W, MAP_CONT_H);
    lv_obj_set_pos(map_container, 30, 44);
    lv_obj_set_style_bg_color(map_container, lv_color_hex(0x060F06), LV_PART_MAIN);
    lv_obj_set_style_border_color(map_container, lv_color_hex(0x00AA00), LV_PART_MAIN);
    lv_obj_set_style_border_width(map_container, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(map_container, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_all(map_container, 0, LV_PART_MAIN);
    lv_obj_remove_flag(map_container, LV_OBJ_FLAG_SCROLLABLE);

    // ── Image d'arriere-plan (cree en PREMIER = z-order le plus bas = derriere les points) ──
    // Initialise le descripteur LVGL une fois pour toutes ; g_map_raw est le tampon de pixels.
    map_img_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    map_img_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    map_img_dsc.header.flags  = 0;
    map_img_dsc.header.w      = MAP_RAW_W;
    map_img_dsc.header.h      = MAP_RAW_H;
    map_img_dsc.header.stride = (uint16_t)(MAP_RAW_W * 2);  // octets par ligne
    map_img_dsc.data_size     = MAP_RAW_BYTES;
    map_img_dsc.data          = g_map_raw;

    map_bg_img = lv_image_create(map_container);
    lv_image_set_src(map_bg_img, &map_img_dsc);
    lv_obj_set_pos(map_bg_img, 0, 0);
    lv_image_set_pivot(map_bg_img, 0, 0);        // ancre en haut a gauche pour le scaling
    lv_image_set_scale(map_bg_img, MAP_BG_SCALE); // 5× : 148→740, 65→325
    lv_obj_add_flag(map_bg_img, LV_OBJ_FLAG_HIDDEN);  // cache jusqu'a reception de la 1re image

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

    // Legend
    lv_obj_t * legend = lv_label_create(screen_map);
    lv_label_set_text(legend, "  A Allie   E Ennemi   O Objectif   F Aerodrome");
    lv_obj_set_style_text_color(legend, lv_color_hex(0x445544), LV_PART_MAIN);
    lv_obj_set_style_text_font(legend, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(legend, 30, 376);

    lv_obj_t * btn_back = lv_btn_create(screen_map);
    lv_obj_align(btn_back, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_set_size(btn_back, 300, 50);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x552200), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_back, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_back, cb_goto_buttons, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_lbl = lv_label_create(btn_back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " PANNEAU DE COMMANDES");
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(back_lbl);
}

// ===== PARSING SERIAL - TELEMETRIE (serial thread, no LVGL) =====
// Fills *out* from a raw telemetry line.  Must NOT call any lv_* function.
// Expected format: SPD:{int}|RPM:{int}|GEAR:{val}|...|STATUS:{0/1}
static void parse_telem_string(const String& data, TelemShared& out) {
    // Store raw line for the telemetry debug label
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

    idx = data.indexOf("TANK:");
    if (idx >= 0) {
        String t = data.substring(idx + 5, data.indexOf("|", idx));
        t.toCharArray(out.tank, sizeof(out.tank));
    } else {
        snprintf(out.tank, sizeof(out.tank), "UNKNOWN");
    }

    idx = data.indexOf("AMMO:");
    out.ammo = (idx >= 0) ? data.substring(idx + 5, data.indexOf("|", idx)).toInt() : 0;

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

// ===== CALLBACK CLIGNOTEMENT ALERTE AMMO (main/M7 thread uniquement) =====
// Appele par un lv_timer toutes les 500 ms quand ammo <= 5.
// Fait clignoter la couleur du contour de la case OBUS RESTANTS.
static void ammo_blink_cb(lv_timer_t * t) {
    g_ammo_blink_on = !g_ammo_blink_on;
    lv_obj_t * box = lv_obj_get_parent(telem_ammo);
    if (box) {
        lv_color_t brd = g_ammo_blink_on ? lv_color_hex(0xFF0000) : lv_color_hex(0x660000);
        lv_obj_set_style_border_color(box, brd, LV_PART_MAIN);
    }
}

// ===== THREAD DE MESURE CPU M7 (osPriorityLow, 2048 o) =====
// Ce thread tourne quand les threads High (serial) et Normal (loop) sont en attente
// (delay, mutex, I/O). Il compte ses iterations sur une fenetre d'une seconde.
// Charge M7 (%) ≈ (1 - iterations_mesurees / iterations_max_observe) * 100
static void cpu_idle_task() {
    uint32_t max_ticks = 0;
    while (true) {
        uint32_t t0 = millis();
        uint32_t n  = 0;
        while ((millis() - t0) < 1000) ++n;
        if (n > max_ticks) max_ticks = n;   // Mise a jour du maximum historique
        if (max_ticks > 0) {
            int32_t load = 100 - (int32_t)((n * 100UL) / max_ticks);
            if (load < 0) load = 0;
            if (load > 100) load = 100;
            g_m7_load_pct = (uint32_t)load;
        }
    }
}

// ===== LVGL UPDATE - TELEMETRIE (main/M7 thread only) =====
// Reads a local copy of TelemShared (no mutex held) and updates LVGL widgets.
static void apply_telem_update(const TelemShared& d) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%.32s | %d km/h | CREW:%d/%d",
             d.tank, d.spd, d.crew, d.crew_total);
    lv_label_set_text(hud_label_btn, buf);

    if (d.online) {
        lv_label_set_text(telem_status, "[OK] PC BRIDGE: ONLINE");
        lv_obj_set_style_text_color(telem_status, lv_color_hex(0x00FF00), LV_PART_MAIN);
    } else {
        lv_label_set_text(telem_status, "[!!] PC BRIDGE: OFFLINE");
        lv_obj_set_style_text_color(telem_status, lv_color_hex(0xFF3300), LV_PART_MAIN);
    }

    snprintf(buf, sizeof(buf), "%d km/h", d.spd);
    lv_label_set_text(telem_spd, buf);

    // ── Obus restants ──
    snprintf(buf, sizeof(buf), "%d", d.ammo);
    lv_label_set_text(telem_ammo, buf);
    lv_obj_t * ammo_box = lv_obj_get_parent(telem_ammo);
    if (ammo_box) {
        if (d.online && d.ammo <= 5) {
            // Alerte : fond rouge, contour plus epais, texte rouge, clignotement
            lv_obj_set_style_bg_color(ammo_box,     lv_color_hex(0x3A0000), LV_PART_MAIN);
            lv_obj_set_style_border_color(ammo_box, lv_color_hex(0xFF0000), LV_PART_MAIN);
            lv_obj_set_style_border_width(ammo_box, 4,                      LV_PART_MAIN);
            lv_obj_set_style_text_color(telem_ammo, lv_color_hex(0xFF4444), LV_PART_MAIN);
            if (g_ammo_blink_timer == NULL) {
                g_ammo_blink_timer = lv_timer_create(ammo_blink_cb, 500, NULL);
            }
        } else {
            // Normal : vert
            lv_obj_set_style_bg_color(ammo_box,     lv_color_hex(0x1C2C1C), LV_PART_MAIN);
            lv_obj_set_style_border_color(ammo_box, lv_color_hex(0x00FF00), LV_PART_MAIN);
            lv_obj_set_style_border_width(ammo_box, 2,                      LV_PART_MAIN);
            lv_obj_set_style_text_color(telem_ammo, lv_color_hex(0x00FF00), LV_PART_MAIN);
            if (g_ammo_blink_timer != NULL) {
                lv_timer_delete(g_ammo_blink_timer);
                g_ammo_blink_timer = NULL;
                g_ammo_blink_on    = false;
            }
        }
    }

    // ── Equipage ──
    snprintf(buf, sizeof(buf), "%d/%d", d.crew, d.crew_total);
    lv_label_set_text(telem_crew_val, buf);
    lv_obj_t * crew_box = lv_obj_get_parent(telem_crew_val);
    if (crew_box) {
        if (d.online && d.crew_total > 0 && d.crew <= 2) {
            // Alerte : fond rouge, contour rouge, texte rouge
            lv_obj_set_style_bg_color(crew_box,         lv_color_hex(0x3A0000), LV_PART_MAIN);
            lv_obj_set_style_border_color(crew_box,     lv_color_hex(0xFF0000), LV_PART_MAIN);
            lv_obj_set_style_text_color(telem_crew_val, lv_color_hex(0xFF4444), LV_PART_MAIN);
        } else {
            // Normal : vert
            lv_obj_set_style_bg_color(crew_box,         lv_color_hex(0x1C2C1C), LV_PART_MAIN);
            lv_obj_set_style_border_color(crew_box,     lv_color_hex(0x00FF00), LV_PART_MAIN);
            lv_obj_set_style_text_color(telem_crew_val, lv_color_hex(0x00FF00), LV_PART_MAIN);
        }
    }

    // ── Charge CPU ──
    snprintf(buf, sizeof(buf), "M7: %lu%%", g_m7_load_pct);
    lv_label_set_text(telem_cpu_m7, buf);
    snprintf(buf, sizeof(buf), "M4: %lu%%", g_m4_load_pct);
    lv_label_set_text(telem_cpu_m4, buf);

    lv_label_set_text(telem_raw, d.raw);
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
    static uint8_t s_b64buf[MAP_B64_MAX];  // 110 000 bytes en BSS
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
    // No setTimeout needed — the serial thread uses non-blocking reads
    Display.begin();
    TouchDetector.begin();

    // Initialise le canal RPC M7 ↔ M4 (demarre le M4 et ouvre la communication).
    RPC.begin();

    COL_DANGER = lv_color_hex(0x8B0000);
    COL_ARMOR  = lv_color_hex(0x4A5D23);
    COL_TECH   = lv_color_hex(0x2F4F4F);
    COL_DARK   = lv_color_hex(0x111111);
    COL_BAR    = lv_color_hex(0x1E1E1E);

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

    // Thread d'inactivite : estime la charge CPU M7 (priorite minimale).
    g_cpu_idle_thread.start(cpu_idle_task);
}

// ===== LOOP (role M7 : rendu & tactile) =====
// The loop exclusively drives LVGL and USB-HID.
// It reads pre-parsed data from the shared structs under a brief mutex lock,
// then applies any pending updates to LVGL widgets — all without blocking on
// serial I/O. delay(5) yields CPU to the USB stack after any HID send, which
// guarantees that HID button presses are reliably delivered to the PC.
void loop() {
    // ── Lecture RPC : charge CPU M4 envoyee par le co-processeur ──
    while (RPC.available()) {
        char c = (char)RPC.read();
        if (c == '\n') {
            rpcRecvBuffer.trim();
            if (rpcRecvBuffer.startsWith("CPU4:")) {
                uint32_t pct = (uint32_t)rpcRecvBuffer.substring(5).toInt();
                if (pct <= 100) g_m4_load_pct = pct;
            }
            rpcRecvBuffer = "";
        } else {
            if (rpcRecvBuffer.length() < RPC_RECV_BUF_MAX) rpcRecvBuffer += c;
            else rpcRecvBuffer = "";
        }
    }

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

    // ── Drive LVGL rendering and touch events ──
    lv_timer_handler();

    // ── Envoi HID differe (apres lv_timer_handler) ──
    // Les callbacks LVGL positionnent un drapeau ; on envoie ici, AVANT delay(5),
    // pour que le stack USB dispose immediatement de 5 ms pour transmettre la trame.
    // C'est la correction du probleme "1 appui sur 20 ignore".
    if (g_hid_char != '\0') {
        char c = g_hid_char;
        g_hid_char = '\0';
        Keyboard.printf("%c", c);
    } else if (g_hid_keycode != 0) {
        uint8_t kc = g_hid_keycode;
        g_hid_keycode = 0;
        Keyboard.key_code(kc);
    }

    // ── Yield to USB stack and other Mbed RTOS tasks ──
    delay(5);
}
