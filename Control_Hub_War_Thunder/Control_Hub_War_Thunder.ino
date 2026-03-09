#include "Arduino_H7_Video.h"
#include "Arduino_GigaDisplayTouch.h"
#include "lvgl.h"
#include "PluggableUSBHID.h"
#include "USBKeyboard.h"

// ===== HARDWARE =====
Arduino_H7_Video Display(800, 480, GigaDisplayShield);
Arduino_GigaDisplayTouch TouchDetector;
USBKeyboard Keyboard;

// ===== ECRANS =====
lv_obj_t * screen_buttons;
lv_obj_t * screen_telem;

// ===== WIDGETS GLOBAUX =====
lv_obj_t * hud_label_btn;
lv_obj_t * telem_spd;
lv_obj_t * telem_rpm;
lv_obj_t * telem_gear;
lv_obj_t * telem_raw;
lv_obj_t * telem_status;

// ===== COULEURS =====
lv_color_t COL_DANGER;
lv_color_t COL_ARMOR;
lv_color_t COL_TECH;
lv_color_t COL_DARK;
lv_color_t COL_BAR;

// ===== CALLBACKS HID =====
static void cb_extincteur(lv_event_t * e)  { Keyboard.printf("6"); }
static void cb_fumigene(lv_event_t * e)    { Keyboard.printf("g"); }
static void cb_artillerie(lv_event_t * e)  { Keyboard.printf("5"); }
static void cb_jumelles(lv_event_t * e)    { Keyboard.printf("b"); }
static void cb_sniper(lv_event_t * e)      { Keyboard.key_code(KEY_SHIFT); }
static void cb_moteur(lv_event_t * e)      { Keyboard.printf("i"); }
static void cb_reparation(lv_event_t * e)  { Keyboard.printf("f"); }

// ===== SWITCH D'ECRANS =====
static void cb_goto_telem(lv_event_t * e) {
    lv_scr_load_anim(screen_telem, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}
static void cb_goto_buttons(lv_event_t * e) {
    lv_scr_load_anim(screen_buttons, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
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
void make_btn(lv_obj_t * parent, const char* icon, const char* label,
              lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
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
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    *label_out = lv_label_create(bar);
    lv_label_set_text(*label_out, "AWAITING TELEMETRY...");
    lv_obj_set_style_text_color(*label_out, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_text_font(*label_out, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(*label_out, LV_ALIGN_LEFT_MID, 10, 0);
}

// ===== ECRAN 1 : COMMANDES =====
void build_screen_buttons() {
    lv_obj_set_style_bg_color(screen_buttons, COL_DARK, LV_PART_MAIN);
    make_hud_bar(screen_buttons, &hud_label_btn);

    lv_obj_t * btn_nav = lv_btn_create(screen_buttons);
    lv_obj_set_pos(btn_nav, 645, 6);
    lv_obj_set_size(btn_nav, 148, 38);
    lv_obj_set_style_bg_color(btn_nav, lv_color_hex(0x00552A), LV_PART_MAIN);
    lv_obj_set_style_radius(btn_nav, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_nav, cb_goto_telem, LV_EVENT_CLICKED, NULL);
    lv_obj_t * nav_lbl = lv_label_create(btn_nav);
    lv_label_set_text(nav_lbl, LV_SYMBOL_RIGHT " TELEMETRY");
    lv_obj_set_style_text_font(nav_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(nav_lbl);

    int W = 228, H = 178;
    int Y1 = 60, Y2 = 255;
    int X1 = 12, X2 = 252, X3 = 492;

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
void make_data_block(lv_obj_t * parent, const char* title_str, lv_obj_t ** val_label,
                     lv_coord_t x, lv_coord_t y) {
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_set_size(box, 220, 110);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1C2C1C), LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(0x00FF00), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 8, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

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

    make_data_block(screen_telem, "VITESSE (km/h)", &telem_spd,  30,  110);
    make_data_block(screen_telem, "REGIME (RPM)",   &telem_rpm,  290, 110);
    make_data_block(screen_telem, "RAPPORT",        &telem_gear, 550, 110);

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

// ===== PARSING SERIAL =====
void parse_and_update(String data) {
    int idx_spd  = data.indexOf("SPD:");
    int idx_tank = data.indexOf("TANK:");
    int idx_crew = data.indexOf("CREW:");

    if (idx_spd >= 0 && idx_tank >= 0 && idx_crew >= 0) {
        String spd  = data.substring(idx_spd  + 4, data.indexOf("|", idx_spd));
        String tank = data.substring(idx_tank + 5, data.indexOf("|", idx_tank));
        String crew = data.substring(idx_crew + 5, data.indexOf("|", idx_crew));
        String hud  = tank + " | " + spd + " km/h | CREW:" + crew;
        lv_label_set_text(hud_label_btn, hud.c_str());
    }

    int idx_status = data.indexOf("STATUS:");
    if (idx_status >= 0) {
        String status = data.substring(idx_status + 7);
        status.trim();
        if (status.startsWith("1")) {
            lv_label_set_text(telem_status, "[OK] PC BRIDGE: ONLINE");
            lv_obj_set_style_text_color(telem_status, lv_color_hex(0x00FF00), LV_PART_MAIN);
        } else {
            lv_label_set_text(telem_status, "[!!] PC BRIDGE: OFFLINE");
            lv_obj_set_style_text_color(telem_status, lv_color_hex(0xFF3300), LV_PART_MAIN);
        }
    }

    lv_label_set_text(telem_raw, data.c_str());

    int idx;
    idx = data.indexOf("SPD:");
    if (idx >= 0) {
        String val = data.substring(idx + 4, data.indexOf("|", idx));
        lv_label_set_text(telem_spd, (val + " km/h").c_str());
    }
    idx = data.indexOf("RPM:");
    if (idx >= 0) {
        String val = data.substring(idx + 4, data.indexOf("|", idx));
        lv_label_set_text(telem_rpm, val.c_str());
    }
    idx = data.indexOf("GEAR:");
    if (idx >= 0) {
        String val = data.substring(idx + 5, data.indexOf("|", idx));
        lv_label_set_text(telem_gear, val.c_str());
    }
}

// ===== SERIAL INPUT BUFFER =====
static String serialBuffer = "";
// Maximum expected message length; longer partial frames are discarded.
static const size_t SERIAL_BUF_MAX = 256;

// ===== SETUP =====
void setup() {
    Serial.begin(115200);
    Serial.setTimeout(100);  // Keep timeout short; non-blocking loop does the real work
    serialBuffer.reserve(SERIAL_BUF_MAX);  // Pre-allocate to avoid repeated heap allocations
    Display.begin();
    TouchDetector.begin();

    COL_DANGER = lv_color_hex(0x8B0000);
    COL_ARMOR  = lv_color_hex(0x4A5D23);
    COL_TECH   = lv_color_hex(0x2F4F4F);
    COL_DARK   = lv_color_hex(0x111111);
    COL_BAR    = lv_color_hex(0x1E1E1E);

    screen_buttons = lv_obj_create(NULL);
    screen_telem   = lv_obj_create(NULL);

    build_screen_buttons();
    build_screen_telem();

    lv_scr_load(screen_buttons);
}

// ===== LOOP =====
void loop() {
    // Non-blocking serial read: accumulate characters until '\n' so that
    // lv_timer_handler() is always called promptly and the receive buffer
    // never stalls while waiting for a newline.
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n') {
            serialBuffer.trim();
            if (serialBuffer.length() > 0) {
                parse_and_update(serialBuffer);
            }
            serialBuffer = "";
        } else {
            // Discard oversized frames to prevent unbounded memory growth
            if (serialBuffer.length() < SERIAL_BUF_MAX) {
                serialBuffer += c;
            } else {
                serialBuffer = "";  // Drop corrupted/runaway frame
            }
        }
    }
    lv_timer_handler_run_in_period(5);
}
