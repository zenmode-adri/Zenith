/* SPDX-License-Identifier: MIT
 * Third tab: voltages and extra speeds, stored in the device tree (needs a restart). Every change goes to a temporary
 * copy first; the platform installs it and arms the boot safety net. */
#include "tuner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fmt_mv(int uv, char *b, int n) {
    if (uv % 1000) snprintf(b, n, "%d.%d mV", uv / 1000, (abs(uv) % 1000) / 100);
    else snprintf(b, n, "%d mV", uv / 1000);
}

static int bin_known(const char *title) {
    if (strcmp(dtb_bin(), "opp-microvolt")) return 1;
    page_info(title, _("Chip bin not detected, so voltages cannot be edited safely. Restart and try again."), 0);
    return 0;
}

static int save_edit(const char *title) {
    int rc = dtb_commit();
    if (rc) {
        page_info(title, rc == DTB_NOT_FROM_IMAGE
            ? _("This console is using a modified device tree. Choose your console again in Zenith DTB Selector, then try again. No changes were made.")
            : _("The change could not be saved. No changes were made."), 0);
        return -1;
    }
    app_status_refresh();
    return 0;
}

static void edit_failed(const char *title) {
    dtb_abort();
    page_info(title, _("The change could not be saved. No changes were made."), 0);
}

static void offer_reboot(const char *title) {
    if (dtb_went_factory()) { app_reboot_offer(title, _("Factory settings restored. Restart to apply.")); return; }
    app_reboot_offer(title, _("Saved. Restart to apply it, then run the stability test. If the console does not boot, R36-TUNER-RECOVERY.txt on the SD card has the steps."));
}

static const char *const NET_WARN[2] = {
    "The automatic revert only works if the boot reaches the operating system. If the console hangs earlier, restore it from a PC with the SD card (see Recovery).",
    "Crashes during gameplay are not reverted automatically. Run the stability test to validate the change.",
};

/* ── Voltage curve: one editor per component ────────────────────────────── */
/* Every frequency with its voltage, edited in place and saved together. Optional frequencies of the platform appear in the
   same list, and can be added or removed. Nothing is written until the changes are reviewed and applied. */
typedef struct {
    long long hz;
    int shown_mhz;
    int cur_uv;          /* 0: not in the table */
    int new_uv;          /* 0: not in the table after applying */
    const RamStep *step; /* optional frequency */
    char node[160];
} CurveRow;

static int row_changed(const CurveRow *r) { return r->cur_uv != r->new_uv; }

static CurveRow *row_of_step(CurveRow *rows, int n, int node_mhz) {
    for (int i = 0; i < n; i++)
        if (rows[i].step && rows[i].step->node_mhz == node_mhz) return &rows[i];
    return NULL;
}

static int curve_load(CurveRow *rows, const char *table, const RamStep *steps, int n_steps) {
    Opp o[MAX_OPP];
    int n = dtb_scan(table, o, MAX_OPP);
    memset(rows, 0, sizeof(CurveRow) * (MAX_OPP + 4));
    for (int i = 0; i < n; i++) {
        rows[i].hz = o[i].hz;
        rows[i].shown_mhz = (int)(o[i].hz / 1000000);
        rows[i].cur_uv = rows[i].new_uv = o[i].uv;
        snprintf(rows[i].node, sizeof(rows[i].node), "%s", o[i].node);
    }
    for (int s = 0; s < n_steps; s++) {
        long long hz = (long long)steps[s].node_mhz * 1000000;
        CurveRow *r = NULL;
        for (int i = 0; i < n; i++) if (rows[i].hz == hz) r = &rows[i];
        if (!r && n < MAX_OPP + 4) {
            r = &rows[n++];
            r->hz = hz;
            snprintf(r->node, sizeof(r->node), "%s/opp-%lld", table, hz);
        }
        if (r) { r->step = &steps[s]; r->shown_mhz = steps[s].shown_mhz; }
    }
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (rows[j].hz > rows[i].hz) { CurveRow t = rows[i]; rows[i] = rows[j]; rows[j] = t; }
    return n;
}

/* Adds or removes an optional frequency, with the ones that depend on it */
static const char *curve_toggle(CurveRow *rows, int n, CurveRow *r) {
    static char msg[100];
    msg[0] = 0;
    if (r->new_uv) {
        r->new_uv = 0;
        for (int i = 0; i < n; i++)
            if (rows[i].step && rows[i].step->need_mhz == r->step->node_mhz) rows[i].new_uv = 0;
    } else {
        CurveRow *need = r->step->need_mhz ? row_of_step(rows, n, r->step->need_mhz) : NULL;
        if (need && !need->new_uv) {
            snprintf(msg, sizeof(msg), _("Add %d MHz first."), need->shown_mhz);
            return msg;
        }
        r->new_uv = r->cur_uv ? r->cur_uv : r->step->def_uv;
    }
    return msg;
}

static void curve_factory(CurveRow *rows, int n, const char *table) {
    char file[300];
    Opp o[MAX_OPP];
    if (plat->dtb_factory(file, sizeof(file))) return;
    int nf = dtb_scan_factory(file, table, o, MAX_OPP);
    for (int i = 0; i < n; i++)
        for (int j = 0; j < nf; j++)
            if (o[j].hz == rows[i].hz && rows[i].new_uv) rows[i].new_uv = o[j].uv;
}

typedef struct { const CurveRow *rows; int n; } CurveCtx;
static void curve_extra(int x, int y, int w, int h, void *ctx);

static int curve_apply(const char *title, const char *table, const CurveRow *rows, int n, int bins) {
    TableRow t[MAX_OPP + 4];
    int nt = 0, experimental = 0;
    memset(t, 0, sizeof(t));
    for (int i = 0; i < n; i++) {
        if (!row_changed(&rows[i])) continue;
        snprintf(t[nt].c[0], 48, "%d MHz", rows[i].shown_mhz);
        if (rows[i].cur_uv) fmt_mv(rows[i].cur_uv, t[nt].c[1], 48); else snprintf(t[nt].c[1], 48, "—");
        if (rows[i].new_uv) fmt_mv(rows[i].new_uv, t[nt].c[2], 48); else snprintf(t[nt].c[2], 48, "%s", _("Removed"));
        t[nt].col = rows[i].new_uv ? C_ACCENT : C_WARN;
        if (rows[i].step && rows[i].step->experimental && rows[i].new_uv) experimental = 1;
        nt++;
    }
    if (!nt) return 0;
    const char *hdr[3] = {_("Frequency"), _("Current"), _("New")};
    const char *warn[3] = {_(NET_WARN[0]), _(NET_WARN[1]), _("Experimental frequency: stability varies between units.")};
    CurveCtx ctx = {rows, n};
    page_table_extra(curve_extra, &ctx);
    int yes = page_table(title, NULL, hdr, t, nt, warn, experimental ? 3 : 2, _("Apply"), _("Cancel"));
    page_table_extra(NULL, NULL);
    if (!yes) return 0;
    if (dtb_begin()) { page_info(title, _("Could not read the device tree. No changes were made."), 0); return 0; }
    for (int i = 0; i < n; i++) {
        const CurveRow *r = &rows[i];
        if (!row_changed(r)) continue;
        int fail = !r->new_uv ? dtb_remove_step(table, r->hz)
                 : !r->cur_uv ? dtb_add_step(table, r->hz, r->new_uv, bins)
                 : dtb_set_uv(r->node, r->new_uv);
        if (fail) { edit_failed(title); return 0; }
    }
    if (save_edit(title)) return 0;
    offer_reboot(title);
    return 1;
}

/* The curve itself: frequency against voltage, current and new. rows go from high to low; the chart reads left to right. */
#define CHART_MIN_H 20   /* grid units; below this it would not be readable */

static void curve_chart(int x, int y, int w, int h, const CurveRow *rows, int n, const CurveRow *selected) {
    ui_round(x, y, w, h, UI_RAD, C_PANEL);
    int lo = 0, hi = 0, fh = ui_fh(F_SM);
    for (int i = 0; i < n; i++) {
        const int v[2] = {rows[i].cur_uv, rows[i].new_uv};
        for (int j = 0; j < 2; j++)
            if (v[j]) { if (!lo || v[j] < lo) lo = v[j]; if (v[j] > hi) hi = v[j]; }
    }
    if (!lo) return;
    lo = (lo - 1) / 50000 * 50000;
    hi = (hi / 50000 + 1) * 50000;

    /* legend */
    int lx = x + UI_PAD, ly = y + UI_GAP;
    ui_round(lx, ly + fh / 2 - U(1), G(4), U(2), U(1), C_MUTED);
    lx += G(4) + G(1);
    lx += ui_text(F_SM, _("Current"), lx, ly, C_DIM) + UI_PAD;
    ui_round(lx, ly + fh / 2 - U(1), G(4), U(3), U(1), C_ACCENT);
    lx += G(4) + G(1);
    ui_text(F_SM, _("New"), lx, ly, C_DIM);

    int pl = x + UI_PAD + G(6), pr = x + w - UI_PAD - G(6), pt = y + G(11), pb = y + h - UI_GAP - fh - G(1), ph = pb - pt;
    float cx[MAX_OPP + 4], cu[MAX_OPP + 4], nu[MAX_OPP + 4];
    const CurveRow *r[MAX_OPP + 4];
    for (int i = 0; i < n; i++) {
        r[i] = &rows[n - 1 - i];
        cx[i] = (float)pl + (n > 1 ? (float)(i * (pr - pl)) / (float)(n - 1) : (float)(pr - pl) / 2);
        cu[i] = r[i]->cur_uv ? (float)pb - (float)(r[i]->cur_uv - lo) * (float)ph / (float)(hi - lo) : -1;
        nu[i] = r[i]->new_uv ? (float)pb - (float)(r[i]->new_uv - lo) * (float)ph / (float)(hi - lo) : -1;
    }
    for (int i = 0; i + 1 < n; i++)
        if (nu[i] >= 0 && nu[i + 1] >= 0) ui_area(cx[i], nu[i], cx[i + 1], nu[i + 1], (float)pb, C_ACCENT, 70, 0);
    ui_line(x + UI_PAD, pb, x + w - UI_PAD, pb, C_LINE);
    for (int i = 0; i + 1 < n; i++)
        if (cu[i] >= 0 && cu[i + 1] >= 0) ui_smooth_line(cx[i], cu[i], cx[i + 1], cu[i + 1], (float)U(2), C_MUTED, 255);
    for (int i = 0; i + 1 < n; i++)
        if (nu[i] >= 0 && nu[i + 1] >= 0) ui_smooth_line(cx[i], nu[i], cx[i + 1], nu[i + 1], (float)U(3), C_ACCENT, 255);
    for (int i = 0; i < n; i++) {
        char a[48];
        snprintf(a, sizeof(a), "%d", r[i]->shown_mhz);
        ui_text_c(F_SM, a, (int)cx[i], pb + G(1), r[i] == selected ? C_TXT : C_MUTED);
        if (nu[i] < 0) continue;
        int on = r[i] == selected, rad = on ? U(7) : U(4), px = (int)cx[i], py = (int)nu[i];
        if (on) ui_rect_a(px - U(1), py, U(2), pb - py, C_TXT, 90);
        ui_round(px - rad, py - rad, 2 * rad, 2 * rad, rad, on ? C_TXT : C_ACCENT);
    }
    /* the label goes last, above everything: the voltage and how much it changes */
    for (int i = 0; i < n; i++) {
        if (r[i] != selected || nu[i] < 0) continue;
        char v[24], d[24], t[56];
        fmt_mv(r[i]->new_uv, v, sizeof(v));
        int delta = r[i]->cur_uv ? r[i]->new_uv - r[i]->cur_uv : 0;
        if (delta) {
            fmt_mv(abs(delta), d, sizeof(d));
            d[strcspn(d, " ")] = 0;
            snprintf(t, sizeof(t), "%s (%s%s)", v, delta > 0 ? "+" : "-", d);
        } else snprintf(t, sizeof(t), "%s", v);
        ui_tag((int)cx[i], (int)nu[i] - U(10), t);
    }
}

/* Same drawing in the review page */
static void curve_extra(int x, int y, int w, int h, void *ctx) {
    const CurveCtx *c = ctx;
    curve_chart(x, y, w, h, c->rows, c->n, NULL);
}

static void curve_editor(const char *title, const char *table, int hi_uv, int all_row, const char *note,
                         const RamStep *steps, int n_steps, int bins) {
    if (!table[0]) { page_info(title, _("This table was not found in the device tree."), 0); return; }
    CurveRow rows[MAX_OPP + 4];
    int n = curve_load(rows, table, steps, n_steps);
    if (!n) { page_info(title, _("This table was not found in the device tree."), 0); return; }
    int sel = 0, first = 0, offset = 0, base = all_row ? 1 : 0, total = n + base;
    const char *flash = "";
    char chip[40];
    snprintf(chip, sizeof(chip), "%s %s", _("Chip"), strcmp(dtb_bin(), "opp-microvolt") ? dtb_bin() + 14 : "--");

    while (ui_running) {
        ui_clear();
        ui_header(title, all_row ? chip : NULL);
        int x = UI_M, w = UW - 2 * x, top = ui_content_top();
        const char *cur_note = flash[0] ? flash : note;
        CurveRow *selr = sel >= base ? &rows[sel - base] : NULL;
        if (!flash[0] && selr && selr->step && selr->step->experimental)
            cur_note = _("Experimental frequency: stability varies between units.");
        /* the note keeps its space, so the table does not jump when moving */
        int note_h = 0;
        if (note) note_h = ui_wrap_h(F_SM, note, w);
        if (n_steps) {
            int e = ui_wrap_h(F_SM, _("Experimental frequency: stability varies between units."), w);
            if (e > note_h) note_h = e;
        }
        if (cur_note && cur_note[0]) { int e = ui_wrap_h(F_SM, cur_note, w); if (e > note_h) note_h = e; }
        int bot = ui_content_bottom() - UI_GAP - (note_h ? note_h + UI_GAP : 0), head = G(7), rh = G(7), foot = G(1);
        int visible = (bot - top - head - foot) / rh;
        if (visible < 1) visible = 1;
        if (visible > total) visible = total;
        if (sel < first) first = sel;
        if (sel >= first + visible) first = sel - visible + 1;

        ui_round(x, top, w, head + visible * rh + foot, UI_RAD, C_PANEL);
        int cx[3] = {x + UI_PAD, x + w / 2, x + w - UI_PAD};
        int ty0 = (rh - ui_fh(F_SM)) / 2;
        ui_text(F_SM, _("Frequency"), cx[0], top + ty0, C_MUTED);
        ui_text_c(F_SM, _("Current"), cx[1], top + ty0, C_MUTED);
        ui_text_r(F_SM, _("New"), cx[2], top + ty0, C_MUTED);
        for (int i = first; i < first + visible; i++) {
            int y = top + head + (i - first) * rh, on = i == sel;
            Col dim = on ? C_ON_ACCENT : C_DIM, txt = on ? C_ON_ACCENT : C_TXT;
            int ty = y + ty0;
            if (on) ui_round(x + G(1), y, w - G(2), rh, U(7), C_ACCENT);
            char a[48], b[48];
            if (i < base) {
                ui_text(F_SM, _("All frequencies"), cx[0], ty, txt);
                ui_text_c(F_SM, "—", cx[1], ty, dim);
                fmt_mv(abs(offset), a, sizeof(a));
                snprintf(b, sizeof(b), "%s%s", offset > 0 ? "+" : offset < 0 ? "-" : "", a);
                ui_text_r(F_SM, b, cx[2], ty, offset && !on ? C_ACCENT : txt);
                continue;
            }
            const CurveRow *r = &rows[i - base];
            snprintf(a, sizeof(a), "%d MHz", r->shown_mhz);
            ui_text(F_SM, a, cx[0], ty, txt);
            if (r->cur_uv) { fmt_mv(r->cur_uv, a, sizeof(a)); ui_text_c(F_SM, a, cx[1], ty, dim); }
            else ui_text_c(F_SM, _("Not added"), cx[1], ty, dim);
            if (r->new_uv) fmt_mv(r->new_uv, b, sizeof(b));
            else snprintf(b, sizeof(b), "%s", r->cur_uv ? _("Removed") : "—");
            ui_text_r(F_SM, b, cx[2], ty, on ? C_ON_ACCENT : !row_changed(r) ? C_DIM : r->new_uv ? C_ACCENT : C_WARN);
        }
        ui_scrollbar(0, top + head, visible * rh, total, visible, first);
        int chart_top = top + head + visible * rh + foot + UI_GAP;
        if (visible == total && bot - chart_top >= G(CHART_MIN_H)) curve_chart(x, chart_top, w, bot - chart_top, rows, n, selr);
        if (cur_note && cur_note[0]) ui_wrap(F_SM, cur_note, x, bot + UI_GAP, w, flash[0] ? C_WARN : C_DIM);

        const char *hints[6][2];
        int nh = 0;
        hints[nh][0] = "<>"; hints[nh++][1] = _("Voltage");
        if (selr && selr->step) { hints[nh][0] = "X"; hints[nh++][1] = selr->new_uv ? _("Remove") : _("Add"); }
        hints[nh][0] = "Y"; hints[nh++][1] = _("Factory");
        hints[nh][0] = "A"; hints[nh++][1] = _("Review");
        hints[nh][0] = "B"; hints[nh++][1] = _("Back");
        hints[nh][0] = hints[nh][1] = NULL;
        ui_hints(hints);
        ui_present();

        Keys k = ui_poll(1000);
        if (!ui_running) return;
        int any = k.up || k.down || k.left || k.right || k.a || k.x || k.y;
        if (any) flash = "";
        if (k.up) sel = (sel + total - 1) % total;
        if (k.down) sel = (sel + 1) % total;
        int dir = k.left ? -UV_STEP_UV : k.right ? UV_STEP_UV : 0;
        if (dir) {
            if (sel < base) {
                offset += dir;
                for (int i = 0; i < n; i++) {
                    if (!rows[i].new_uv) continue;
                    int v = rows[i].new_uv + dir;
                    rows[i].new_uv = v < OPP_FLOOR_UV ? OPP_FLOOR_UV : v > hi_uv ? hi_uv : v;
                }
            } else if (selr->new_uv) {
                int v = selr->new_uv + dir;
                selr->new_uv = v < OPP_FLOOR_UV ? OPP_FLOOR_UV : v > hi_uv ? hi_uv : v;
            }
        }
        if (k.x && selr && selr->step) flash = curve_toggle(rows, n, selr);
        if (k.y) { curve_factory(rows, n, table); offset = 0; }
        if (k.a) {
            int changed = 0;
            for (int i = 0; i < n; i++) changed |= row_changed(&rows[i]);
            if (!changed) { flash = _("No changes to apply."); continue; }
            if (curve_apply(title, table, rows, n, bins)) return;
        }
        if (k.b || k.sel) {
            int changed = 0;
            for (int i = 0; i < n; i++) changed |= row_changed(&rows[i]);
            if (!changed || page_confirm(title, _("Discard your changes?"), NULL, 0, _("Discard"), _("Keep editing"), 1)) return;
        }
    }
}

static void edit_cpu(void) {
    const char *title = _("CPU voltage curve");
    if (!bin_known(title)) return;
    curve_editor(title, dtb_cpu_table(), CPU_MAX_UV, 1, NULL, NULL, 0, 1);
}

static void edit_gpu(void) {
    const char *title = _("GPU voltage curve");
    if (!bin_known(title)) return;
    curve_editor(title, dtb_gpu_table(), LOGIC_MAX_UV, 1,
                 _("GPU and memory share a power rail (vdd_logic): the higher voltage of the two applies."),
                 plat->gpu_steps, plat->n_gpu_steps, 1);
}

static void edit_ram(void) {
    const char *title = _("Memory frequencies and voltages");
    dtb_use_ram_bin();
    if (plat->dmc_bins && !bin_known(title)) { dtb_use_bin(NULL); return; }
    curve_editor(title, dtb_ram_table(), LOGIC_MAX_UV, 0,
                 _("GPU and memory share a power rail (vdd_logic): the higher voltage of the two applies."),
                 plat->ram_steps, plat->n_ram_steps, plat->dmc_bins);
    dtb_use_bin(NULL);
}

/* ── CPU overclock with the teacupx kernel (dArkOSRE) ───────────────────── */
#define BOOTINI "/boot/boot.ini"
#define OC_PENDING "/boot/.r36_oc_boot_pending"
#define OC_BOOTING "/boot/.r36_oc_boot_booting"

/* Full speed start: if it never finishes, the next start puts boot_cpufreq back to 1296. Only that value is touched. */
static int oc_setup_safety(void) {
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s", rp("/tmp/r36_oc_safety_setup.sh"));
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f,
        "#!/bin/bash\n"
        "cat > /usr/local/bin/r36-oc-safety.sh << 'SAFEEOF'\n"
        "#!/bin/bash\n"
        "BOOTINI=" BOOTINI "\n"
        "BOOTING=" OC_BOOTING "\n"
        "PENDING=" OC_PENDING "\n"
        "if [ -f \"$BOOTING\" ]; then\n"
        "  sed -Ei 's/boot_cpufreq=[0-9]+/boot_cpufreq=1296/' \"$BOOTINI\" 2>/dev/null\n"
        "  rm -f \"$BOOTING\" \"$PENDING\"\n"
        "  sync\n"
        "  exit 0\n"
        "fi\n"
        "if [ -f \"$PENDING\" ]; then\n"
        "  mv \"$PENDING\" \"$BOOTING\" && sync\n"
        "fi\n"
        "exit 0\n"
        "SAFEEOF\n"
        "chmod +x /usr/local/bin/r36-oc-safety.sh\n"
        "printf '[Unit]\\nDescription=R36 OC boot safety\\n"
            "DefaultDependencies=no\\nAfter=local-fs.target\\nBefore=basic.target\\n\\n"
            "[Service]\\nType=oneshot\\n"
            "ExecStart=/usr/local/bin/r36-oc-safety.sh\\nRemainAfterExit=yes\\n\\n"
            "[Install]\\nWantedBy=basic.target\\n'"
        " > /etc/systemd/system/r36-oc-safety.service\n"
        "cat > /usr/local/bin/r36-oc-ok.sh << 'OKEOF'\n"
        "#!/bin/bash\n"
        "rm -f " OC_BOOTING " " OC_PENDING "\n"
        "sync\n"
        "OKEOF\n"
        "chmod +x /usr/local/bin/r36-oc-ok.sh\n"
        "printf '[Unit]\\nDescription=R36 OC boot OK\\nAfter=emulationstation.service\\n\\n"
            "[Service]\\nType=oneshot\\n"
            "ExecStart=/usr/local/bin/r36-oc-ok.sh\\nRemainAfterExit=yes\\n\\n"
            "[Install]\\nWantedBy=multi-user.target\\n'"
        " > /etc/systemd/system/r36-oc-ok.service\n"
        "systemctl daemon-reload 2>/dev/null\n"
        "systemctl enable r36-oc-safety.service 2>/dev/null\n"
        "systemctl enable r36-oc-ok.service 2>/dev/null\n");
    fclose(f);
    if (getenv("R36T_ROOT")) return getenv("R36T_FAIL_SAFETY") ? -1 : 0;   /* tests: the service cannot be installed there */
    return sys_root("bash %s", tmp);
}

static void cpu_oc(void) {
    const char *title = _("CPU overclock");
    if (!sys_exists(BOOTINI)) { page_info(title, _("boot.ini not found. This requires the teacupx kernel."), 0); return; }
    if (!bin_known(title)) return;
    const char *table = dtb_cpu_table();
    if (!table[0]) { page_info(title, _("This table was not found in the device tree."), 0); return; }

    static const int add[] = {1512, 1488, 1464, 1440, 1368, CPU_STOCK_MAX_MHZ};   /* the last one takes the overclock off */
    int mhz[MAX_OPP + 6], n = 0;
    {
        Opp o[MAX_OPP];
        int ns = dtb_scan(table, o, MAX_OPP);
        for (int i = 0; i < ns; i++) if (o[i].hz / 1000000 > CPU_STOCK_MAX_MHZ) mhz[n++] = (int)(o[i].hz / 1000000);
        for (int i = 0; i < 6; i++) {
            int dup = 0;
            for (int j = 0; j < n; j++) if (mhz[j] == add[i]) dup = 1;
            if (!dup) mhz[n++] = add[i];
        }
        for (int i = 0; i < n - 1; i++)
            for (int j = i + 1; j < n; j++)
                if (mhz[j] > mhz[i]) { int t = mhz[i]; mhz[i] = mhz[j]; mhz[j] = t; }
    }
    char b[32];
    sys_cap(b, sizeof(b), "grep -o 'max_cpufreq=[0-9]*' '%s' 2>/dev/null | head -1 | cut -d= -f2", rp(BOOTINI));
    int cur = atoi(b);
    if (cur > 5000) cur /= 1000;

    Row rows[MAX_OPP + 6];
    memset(rows, 0, sizeof(rows));
    int sel = 0;
    for (int i = 0; i < n; i++) {
        snprintf(rows[i].label, sizeof(rows[i].label), "%d MHz", mhz[i]);
        if (mhz[i] == cur) { snprintf(rows[i].value, sizeof(rows[i].value), "%s", _("active")); sel = i; }
    }
    sel = page_list(title, _("Maximum frequency"), rows, n, sel);
    if (sel < 0) return;
    int want = mhz[sel], boot = CPU_STOCK_MAX_MHZ;

    /* Starting at full speed is only offered once the CPU test has passed at that speed */
    char r1[16];
    sys_cap(r1, sizeof(r1), "dmesg 2>/dev/null | grep -c '\\[oga-avs\\]'");
    if (want > CPU_STOCK_MAX_MHZ && atoi(r1) > 0 && bench_passed_at(want)) {
        Row m[2];
        memset(m, 0, sizeof(m));
        snprintf(m[0].label, sizeof(m[0].label), "%s", _("Safe boot (1296 MHz)"));
        snprintf(m[0].desc, sizeof(m[0].desc), "%s", _("Recommended. Raise the limit after booting."));
        snprintf(m[1].label, sizeof(m[1].label), "%s", _("Boot at this frequency"));
        snprintf(m[1].desc, sizeof(m[1].desc), "%s", _("Returns to 1296 MHz if a boot that reaches the operating system does not finish. Otherwise restore boot.ini from a PC."));
        int c = page_list(_("Boot mode"), _("CPU benchmark completed at this frequency."), m, 2, 0);
        if (c < 0) return;
        if (c == 1) boot = want;
    }
    char msg[100];
    snprintf(msg, sizeof(msg), "%d MHz  ·  boot %d MHz", want, boot);
    const char *warn[1] = {_(NET_WARN[0])};
    if (!page_confirm(title, msg, warn, 1, _("Apply"), _("Cancel"), 1)) return;

    /* The five extra speeds */
    if (want > CPU_STOCK_MAX_MHZ && !dtb_has_step(table, 1368000000LL)) {
        static const struct { long long hz; int l2, l3; } oc[] = {
            {1368000000LL, 1300000, 1250000}, {1440000000LL, 1300000, 1250000}, {1464000000LL, 1300000, 1250000},
            {1488000000LL, 1300000, 1250000}, {1512000000LL, 1300000, 1250000},
        };
        if (dtb_begin()) { page_info(title, _("Could not read the device tree. No changes were made."), 0); return; }
        const char *d = dtb_tmp();
        for (int i = 0; i < 5; i++) {
            char node[120];
            snprintf(node, sizeof(node), "%s/opp-%lld", table, oc[i].hz);
            int fail = dtb_has_step(table, oc[i].hz) ? 0 : sys_root("fdtput -c '%s' '%s'", d, node);
            fail |= sys_root("fdtput -t u '%s' '%s' opp-hz 0 %lld", d, node, oc[i].hz);
            fail |= sys_root("fdtput -t u '%s' '%s' clock-latency-ns 40000", d, node);
            fail |= sys_root("fdtput -t u '%s' '%s' opp-microvolt 1350000 1350000 1350000", d, node);
            fail |= sys_root("fdtput -t u '%s' '%s' opp-microvolt-L0 1350000 1350000 1350000", d, node);
            fail |= sys_root("fdtput -t u '%s' '%s' opp-microvolt-L1 1350000 1350000 1350000", d, node);
            fail |= sys_root("fdtput -t u '%s' '%s' opp-microvolt-L2 %d %d 1350000", d, node, oc[i].l2, oc[i].l2);
            fail |= sys_root("fdtput -t u '%s' '%s' opp-microvolt-L3 %d %d 1350000", d, node, oc[i].l3, oc[i].l3);
            if (fail) { edit_failed(title); return; }
        }
        if (save_edit(title)) return;
    }

    /* boot.ini: one rescue copy, then only these two values */
    char bi[300];   /* its own copy: rp() reuses its buffers, and the commands below call it again */
    snprintf(bi, sizeof(bi), "%s", rp(BOOTINI));
    if (sys_root("[ -f '%s.oc_bak' ] || { cp '%s' '%s.oc_bak.new' && cmp -s '%s' '%s.oc_bak.new' && mv '%s.oc_bak.new' '%s.oc_bak'; }", bi, bi, bi, bi, bi, bi, bi)) {
        page_info(title, _("The change could not be saved. No changes were made."), 0);
        return;
    }
    /* The safety net goes first: boot.ini does not change without it on the card */
    if (boot != CPU_STOCK_MAX_MHZ) {
        if (oc_setup_safety() || sys_root("touch '%s' && sync", rp(OC_PENDING))) {
            page_info(title, _("The change could not be saved. No changes were made."), 0);
            return;
        }
    } else {
        sys_root("rm -f '%s' '%s'; sync", rp(OC_PENDING), rp(OC_BOOTING));
    }
    int fail = sys_root("sed -i '/^max_cpufreq=/d; /^boot_cpufreq=/d' '%s' && sed -Ei 's/ *max_cpufreq=[0-9]+//g; s/ *boot_cpufreq=[0-9]+//g' '%s'", bi, bi);
    fail |= sys_root("sed -Ei 's/(setenv bootargs \"[^\"]*)\"/\\1 max_cpufreq=%d boot_cpufreq=%d\"/' '%s' && grep -q 'max_cpufreq=%d boot_cpufreq=%d' '%s' && sync", want, boot, bi, want, boot, bi);
    if (fail) {
        sys_root("cp '%s.oc_bak' '%s'; rm -f '%s' '%s'; sync", bi, bi, rp(OC_PENDING), rp(OC_BOOTING));
        page_info(title, _("The change could not be saved. No changes were made."), 0);
        return;
    }
    offer_reboot(title);
}

/* ── Looking and undoing ────────────────────────────────────────────────── */
static int add_table_rows(TableRow *rows, int at, const char *name, const char *table) {
    Opp d[MAX_OPP], k[MAX_OPP];
    if (!table[0]) return at;
    int nd = dtb_scan(table, d, MAX_OPP), nk = dtb_scan_live(table, k, MAX_OPP);
    for (int i = 0; i < nd; i++) {
        snprintf(rows[at].c[0], 48, "%s %lld MHz", name, d[i].hz / 1000000);
        fmt_mv(d[i].uv, rows[at].c[1], 48);
        snprintf(rows[at].c[2], 48, "--");
        rows[at].col = C_WARN;
        for (int j = 0; j < nk; j++)
            if (k[j].hz == d[i].hz) {
                fmt_mv(k[j].uv, rows[at].c[2], 48);
                if (k[j].uv == d[i].uv) rows[at].col = C_TXT;
            }
        at++;
    }
    return at;
}

static void voltage_table(void) {
    TableRow rows[MAX_OPP * 3];
    memset(rows, 0, sizeof(rows));
    int n = add_table_rows(rows, 0, "CPU", dtb_cpu_table());
    n = add_table_rows(rows, n, "GPU", dtb_gpu_table());
    dtb_use_ram_bin();
    n = add_table_rows(rows, n, "RAM", dtb_ram_table());
    dtb_use_bin(NULL);
    const char *hdr[3] = {_("Frequency"), _("Saved"), _("Active")};
    if (!n) { page_info(_("Voltage summary"), _("This table was not found in the device tree."), 0); return; }
    page_table(_("Voltage summary"), NULL, hdr, rows, n, NULL, 0, NULL, NULL);
}

static void undo_all(void) {
    const char *title = _("Restore factory settings");
    app_status_refresh();
    if (!app_dtb.tuned && !app_dtb.overclocked && app_dtb.net == NET_NONE) { page_info(title, _("Already on factory settings."), 0); return; }
    if (!page_confirm(title, _("Restore factory settings? All voltage and frequency changes will be lost."),
                      NULL, 0, _("Restore"), _("Cancel"), 1)) return;
    if (plat->dtb_revert()) { page_info(title, _("Could not restore factory settings."), 0); return; }
    app_reboot_offer(title, _("Factory settings restored. Restart to apply."));
}

static void recovery(void) {
    const char *steps[8];
    int n = 0;
    for (int i = 0; i < 8 && plat->recovery[i]; i++) steps[n++] = _(plat->recovery[i]);
    page_steps(_("Recovery"), steps, n);
}

/* ── The tab ────────────────────────────────────────────────────────────── */
enum { A_CPU, A_GPU, A_RAM, A_OC, A_TABLE, A_UNDO, A_RECOVERY };

void tab_advanced(Keys k, int top, int bottom) {
    static int sel, first;
    int ids[8], n = 0;
    Row rows[8];
    memset(rows, 0, sizeof(rows));
#define ADD(id, lbl, dsc) do { ids[n] = (id); snprintf(rows[n].label, sizeof(rows[n].label), "%s", (lbl)); \
        snprintf(rows[n].desc, sizeof(rows[n].desc), "%s", (dsc)); n++; } while (0)
    ADD(A_CPU, _("CPU voltage curve"), _("Voltage for each CPU frequency, individually or all at once."));
    ADD(A_GPU, _("GPU voltage curve"), _("Voltage for each GPU frequency, individually or all at once."));
    ADD(A_RAM, _("Memory frequencies and voltages"), _("Voltage for each frequency, and optional higher frequencies."));
    if (plat->has_bootini_oc) ADD(A_OC, _("CPU overclock (teacupx)"), _("Requires the teacupx kernel. Up to 1512 MHz."));
    ADD(A_TABLE, _("Voltage summary"), _("Saved values compared with those active in the kernel."));
    ADD(A_UNDO, _("Restore factory settings"), _("Discards every voltage and frequency change stored on the SD card."));
    ADD(A_RECOVERY, _("Recovery"), _("How to restore from a PC with the SD card if the console does not boot."));
#undef ADD

    if (k.up) sel = (sel + n - 1) % n;
    if (k.down) sel = (sel + 1) % n;
    if (k.a) {
        switch (ids[sel]) {
        case A_CPU: edit_cpu(); break;
        case A_GPU: edit_gpu(); break;
        case A_RAM: edit_ram(); break;
        case A_OC: cpu_oc(); break;
        case A_TABLE: voltage_table(); break;
        case A_UNDO: undo_all(); break;
        case A_RECOVERY: recovery(); break;
        }
    }

    app_chrome();
    int y = top;
    if (app_dtb.net == NET_UNDONE) {
        y += ui_banner(UI_M, y, UW - 2 * UI_M, C_BAD, _("The last change prevented a complete boot and was reverted.")) + G(1);
    } else if (app_dtb.reboot_needed) {
        y += ui_banner(UI_M, y, UW - 2 * UI_M, C_WARN, _("Restart to apply your changes.")) + G(1);
    }
    ui_list(rows, n, sel, &first, y, bottom);
    const char *hints[5][2] = {{"A", _("Select")}, {"L R", _("Tab")}, {"B", _("Back")}, {NULL, NULL}};
    ui_hints(hints);
}
