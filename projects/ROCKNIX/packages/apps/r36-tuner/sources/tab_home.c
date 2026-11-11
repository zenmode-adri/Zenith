/* SPDX-License-Identifier: MIT
 * First tab: how the console is doing right now, and the one choice most people need. */
#include "tuner.h"
#include <stdio.h>

static const char *desc_of(int p) {
    switch (p) {
    case PROFILE_SAVE: return _("Reduces maximum frequencies to extend battery life. Demanding games will run slower.");
    case PROFILE_PERF: return _("Maximum frequencies while playing. Best for N64, PSP and Dreamcast. Higher temperature and battery use.");
    default: return _("Frequencies adapt automatically to the load. Recommended for most games.");
    }
}

void tab_home(Keys k, int top, int bottom) {
    static int cursor = -1;
    Profile cur = profile_current();
    if (cursor < 0) cursor = cur == PROFILE_CUSTOM ? PROFILE_BALANCED : (int)cur;
    if (k.left && cursor > 0) cursor--;
    if (k.right && cursor < 2) cursor++;
    if (k.a && (Profile)cursor != cur) {
        profile_apply((Profile)cursor);
        cur = profile_current();
        state_refresh();
    }

    app_chrome();
    int x = UI_M, w = UW - 2 * x, gap = UI_GAP, y = top;

    /* A message only when there is something to do about it */
    const char *msg = NULL;
    Col mc = C_WARN;
    if (st.temp >= 85) { msg = _("Critical temperature: the system is limiting frequencies."); mc = C_BAD; }
    else if (app_dtb.net == NET_UNDONE) { msg = _("The last change prevented a complete boot and was reverted."); mc = C_BAD; }
    else if (app_dtb.net == NET_STALE) msg = _("A system update restored the factory configuration. Apply your changes again.");
    else if (app_dtb.reboot_needed) msg = _("Restart to apply your changes.");
    else if (app_dtb.net == NET_TRIAL) msg = _("Changes not validated. Run the stability test.");
    else if (st.temp >= 75) msg = _("High temperature.");
    if (msg) y += ui_banner(x, y, w, mc, msg) + gap;

    /* The profile part has a fixed size; the cards share the rest */
    int seg_h = G(11), desc_h = 0;
    for (int i = 0; i < 3; i++) {
        int dh = ui_wrap_h(F_SM, desc_of(i), w);
        if (dh > desc_h) desc_h = dh;
    }
    int prof_h = ui_fh(F_SM) + G(1) + seg_h + UI_GAP + desc_h;
    int avail = bottom - y - prof_h - gap;
    int row2 = avail * 46 / 100 - gap / 2, row1 = avail - row2 - gap;
    ui_check_fits("home cards", G(14), row2);

    char v[48], s[48];
    snprintf(v, sizeof(v), "%.0f °C", st.temp);
    ui_card(ui_col(0), y, ui_span(0, 6), row1, _("Temperature"), v, C_TXT, NULL, st.h_temp, 30, 90,
             st.temp >= 85 ? C_BAD : st.temp >= 75 ? C_WARN : C_OK, "°C", 85);
    int bx = ui_col(6), bw = ui_span(6, 6);
    if (st.bat_pct >= 0) {
        snprintf(v, sizeof(v), "%d %%", st.bat_pct);
        s[0] = 0;
        if (st.bat_charging) snprintf(s, sizeof(s), "%s", _("charging"));
        else if (st.bat_w >= 0) snprintf(s, sizeof(s), "%.1f W", st.bat_w);
        ui_card(bx, y, bw, row1, _("Battery"), v, C_TXT, s, NULL, 0, 0, C_OK, NULL, 0);
        ui_bar(bx + UI_PAD, y + row1 - UI_GAP - UI_GAP, bw - 2 * UI_PAD, UI_GAP, (float)st.bat_pct / 100.0f,
               st.bat_pct <= 15 ? C_BAD : C_OK);
    } else {
        ui_card(bx, y, bw, row1, _("Battery"), "--", C_TXT, NULL, NULL, 0, 0, C_OK, NULL, 0);
    }
    y += row1 + gap;

    const struct { const char *t; int cur, max; const float *h; } sp[3] = {
        {"CPU", st.cpu_mhz, st.cpu_max_mhz, st.h_cpu},
        {"GPU", st.gpu_mhz, st.gpu_max_mhz, st.h_gpu},
        {_("Memory"), st.ram_mhz, st.ram_max_mhz, st.h_ram},
    };
    for (int i = 0; i < 3; i++) {
        snprintf(v, sizeof(v), "%d MHz", sp[i].cur);
        snprintf(s, sizeof(s), "%s %d", _("max"), sp[i].max);
        ui_card(ui_col(i * 4), y, ui_span(i * 4, 4), row2, sp[i].t, v, C_TXT, s, sp[i].h, 0, sp[i].max > 0 ? (float)sp[i].max : 1, C_ACCENT, "MHz", 0);
    }
    y += row2 + gap;

    /* Profile */
    ui_text(F_SM, _("Profile"), x, y, C_DIM);
    if (cur == PROFILE_CUSTOM) ui_text_r(F_SM, _("Custom settings active."), x + w, y, C_MUTED);
    y += ui_fh(F_SM) + G(1);
    const char *names[3] = {_("Power saver"), _("Balanced"), _("Performance")};
    for (int i = 0; i < 3; i++) {
        int sx = ui_col(i * 4), cw = ui_span(i * 4, 4), active = (Profile)i == cur, focus = i == cursor, line = U(2);
        ui_round(sx, y, cw, seg_h, UI_RAD, focus ? C_TXT : active ? C_ACCENT : C_PANEL2);
        if (focus) ui_round(sx + line, y + line, cw - 2 * line, seg_h - 2 * line, UI_RAD - line, active ? C_ACCENT : C_PANEL2);
        ui_text_c(F_MD, names[i], sx + cw / 2, y + (seg_h - ui_fh(F_MD)) / 2, active ? C_ON_ACCENT : C_TXT);
    }
    y += seg_h + UI_GAP;
    ui_wrap(F_SM, desc_of(cursor), x, y, w, C_DIM);

    const char *hints[5][2] = {{"<>", _("Profile")}, {"A", _("Apply")}, {"L R", _("Tab")}, {"B", _("Exit")}, {NULL, NULL}};
    ui_hints(hints);
}
