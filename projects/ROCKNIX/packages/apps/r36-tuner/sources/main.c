/* SPDX-License-Identifier: MIT
 * R36 Tuner Next. Tabs and the main loop; see tuner.h for the map of the program. */
#include "tuner.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

DtbStatus app_dtb;
static int tab;

typedef void (*TabFn)(Keys, int, int);
static const struct { const char *name; TabFn fn; } TABS[] = {
    {"Home", tab_home}, {"Settings", tab_tune}, {"Advanced", tab_advanced}, {"Tests", tab_tests},
};
#define N_TABS ((int)(sizeof(TABS) / sizeof(TABS[0])))

static void on_signal(int s) { (void)s; ui_running = 0; }

void app_status_refresh(void) { plat->dtb_status(&app_dtb); }

void app_reboot_offer(const char *title, const char *msg) {
    app_status_refresh();
    if (page_confirm(title, msg, NULL, 0, _("Restart now"), _("Later"), 0)) {
        page_busy(title, _("Restart now"));
        plat->reboot();
    }
}

#define TAB_H 9   /* grid units */

/* Tabs start below this */
static int tabs_bottom(void) { return ui_content_top() + G(TAB_H) + UI_GAP; }

void app_chrome(void) {
    char right[80], bat[24] = "";
    ui_clear();
    if (st.bat_pct >= 0) snprintf(bat, sizeof(bat), "  ·  %d%%%s", st.bat_pct, st.bat_charging ? "+" : "");
    snprintf(right, sizeof(right), "%s%s%s%s", plat->name, st.gpu_driver[0] ? "  ·  " : "", st.gpu_driver, bat);
    ui_header("R36 Tuner", right);

    int y = ui_content_top(), h = G(TAB_H);
    ui_round(UI_M, y, UW - 2 * UI_M, h, UI_RAD, C_PANEL);
    for (int i = 0; i < N_TABS; i++) {
        int x = ui_col(i * 3), cw = ui_span(i * 3, 3);
        if (i == tab) ui_round(x, y, cw, h, UI_RAD, C_ACCENT);
        ui_text_c(F_MD, _(TABS[i].name), x + cw / 2, y + (h - ui_fh(F_MD)) / 2, i == tab ? C_ON_ACCENT : C_DIM);
    }
}

int main(int argc, char **argv) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    plat_init();

    /* hides the launcher's command echo before the first frame */
    int t = open("/dev/tty1", O_WRONLY);
    if (t >= 0) { if (write(t, "\033c", 2) < 0) { /* no console: nothing to clear */ } close(t); }

    int rc = ui_init(argc, argv);
    if (rc) {
        fprintf(stderr, "r36-tuner: could not start (%d): %s\n", rc, rc == -2 ? "no font found" : SDL_GetError());
        return 1;
    }
    i18n_init();
    state_refresh();
    app_status_refresh();

    Keys k;
    memset(&k, 0, sizeof(k));
    Uint32 last = SDL_GetTicks();
    while (ui_running) {
        Uint32 now = SDL_GetTicks();
        if (now - last >= 1000) { state_refresh(); last = now; }
        if (k.l) tab = (tab + N_TABS - 1) % N_TABS;
        if (k.r) tab = (tab + 1) % N_TABS;
        if (k.l || k.r) app_status_refresh();
        TABS[tab].fn(k, tabs_bottom(), ui_content_bottom() - UI_GAP);
        ui_present();
        k = ui_poll(1000);
        if (k.b && tab == 0) break;
        if (k.b) { tab = 0; memset(&k, 0, sizeof(k)); }
    }
    if (app_dtb.net == NET_UNDONE || app_dtb.net == NET_STALE) plat->dtb_ack();
    ui_quit();
    return 0;
}
