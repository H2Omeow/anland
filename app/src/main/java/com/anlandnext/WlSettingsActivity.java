package com.anlandnext;

import android.app.Activity;
import android.os.Bundle;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;

/** Window behavior settings.
 *  IME mode → SharedPreferences (APK-local, read by WlWindowActivity);
 *  display zoom → daemon config.json (the daemon is the single source of
 *  truth, read/written over binder, #31).
 *  (The exit-behavior setting is gone: swiping away / killing the background
 *  is fixed to minimize-and-keep-alive, and the only close entry is the
 *  window-list long-press menu — all decisions live in the daemon, the APK
 *  has no policy left to configure.) */
public class WlSettingsActivity extends Activity {
    private android.widget.LinearLayout root;
    private android.os.Handler h;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle(R.string.settings_title);

        root = new android.widget.LinearLayout(this);
        root.setOrientation(android.widget.LinearLayout.VERTICAL);
        root.setPadding(48, 64, 48, 48);

        /* ---- IME display mode ---- */
        TextView imeTip = new TextView(this);
        imeTip.setText(R.string.ime_mode_tip);
        root.addView(imeTip);

        RadioGroup imeRg = new RadioGroup(this);
        RadioButton inset = new RadioButton(this);
        inset.setId(3);
        inset.setText(R.string.ime_mode_inset);
        RadioButton overlay = new RadioButton(this);
        overlay.setId(4);
        overlay.setText(R.string.ime_mode_overlay);

        imeRg.addView(inset);
        imeRg.addView(overlay);
        int imeMode = getSharedPreferences("awl", MODE_PRIVATE).getInt("ime_mode", 0);
        (imeMode != 0 ? overlay : inset).setChecked(true);
        imeRg.setOnCheckedChangeListener((g, checkedId) ->
                getSharedPreferences("awl", MODE_PRIVATE).edit()
                        .putInt("ime_mode", checkedId == 4 ? 1 : 0)
                        .apply());
        root.addView(imeRg);

        /* ---- Display zoom (arbitrary ratio, daemon-side; #31) ---- */
        android.widget.Space gap2 = new android.widget.Space(this);
        gap2.setMinimumHeight(64);
        root.addView(gap2);

        TextView zoomTip = new TextView(this);
        zoomTip.setText(R.string.zoom_tip);
        root.addView(zoomTip);

        TextView zoomVal = new TextView(this);
        zoomVal.setTextSize(20);
        root.addView(zoomVal);

        int got = WlBinder.configGet("zoom");
        final int[] cur = {(got < 50 || got > 300) ? 100 : got};   /* daemon down / unknown → 100 */

        android.widget.SeekBar seek = new android.widget.SeekBar(this);
        seek.setMax(250);          /* progress = pct - 50 → any value in 50..300 */
        seek.setProgress(cur[0] - 50);
        root.addView(seek);

        /* 200ms drag debounce + immediate on release/preset — applies
         * dynamically without bombarding per tick (each set = a
         * preferred_scale broadcast + re-configure of every window) */
        android.os.Handler h = new android.os.Handler(getMainLooper());
        this.h = h;
        final Runnable[] pending = new Runnable[1];
        Runnable apply = () -> WlBinder.configSet("zoom", cur[0]);
        java.util.function.IntConsumer showZoom = pct ->
                zoomVal.setText(pct + "%  =  " + (pct / 100.0) + "×");

        java.util.function.IntConsumer setZoom = pct -> {
            cur[0] = pct;
            seek.setProgress(pct - 50);
            showZoom.accept(pct);
            if (pending[0] != null) h.removeCallbacks(pending[0]);
            h.post(apply);   /* preset: immediate */
        };
        seek.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(android.widget.SeekBar sb, int prog, boolean fromUser) {
                if (!fromUser) return;
                cur[0] = prog + 50;
                showZoom.accept(cur[0]);
                if (pending[0] != null) h.removeCallbacks(pending[0]);
                int pct = cur[0];
                pending[0] = () -> WlBinder.configSet("zoom", pct);
                h.postDelayed(pending[0], 200);
            }
            @Override public void onStartTrackingTouch(android.widget.SeekBar sb) { }
            @Override public void onStopTrackingTouch(android.widget.SeekBar sb) {
                if (pending[0] != null) h.removeCallbacks(pending[0]);
                pending[0] = null;
                h.post(apply);   /* release: immediate */
            }
        });
        showZoom.accept(cur[0]);

        android.widget.LinearLayout presets = new android.widget.LinearLayout(this);
        presets.setOrientation(android.widget.LinearLayout.HORIZONTAL);
        int[] ratios = {100, 150, 175, 200, 250};
        for (int r : ratios) {
            android.widget.Button btn = new android.widget.Button(this);
            btn.setText(r + "%");
            btn.setOnClickListener(v -> setZoom.accept(r));
            android.widget.LinearLayout.LayoutParams lp =
                    new android.widget.LinearLayout.LayoutParams(
                            0, android.widget.LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
            presets.addView(btn, lp);
        }
        root.addView(presets);

        /* ---- Window scaling mode (fixed-size windows under resize, daemon
         *      config scale_mode; #34 — stretch/fit-letterbox/centered 1:1) ---- */
        android.widget.Space gap25 = new android.widget.Space(this);
        gap25.setMinimumHeight(64);
        root.addView(gap25);

        TextView scaleTip = new TextView(this);
        scaleTip.setText(R.string.scale_mode_tip);
        root.addView(scaleTip);

        RadioGroup scaleRg = new RadioGroup(this);
        RadioButton stretch = new RadioButton(this);
        stretch.setId(5);   /* unique within this activity (IME group uses 3/4) */
        stretch.setText(R.string.scale_mode_stretch);
        RadioButton fit = new RadioButton(this);
        fit.setId(6);
        fit.setText(R.string.scale_mode_fit);
        RadioButton center = new RadioButton(this);
        center.setId(7);
        center.setText(R.string.scale_mode_center);
        scaleRg.addView(stretch);
        scaleRg.addView(fit);
        scaleRg.addView(center);
        int gotMode = WlBinder.configGet("scale_mode");
        if (gotMode < 0 || gotMode > 2) gotMode = 0;   /* daemon down / unknown → stretch */
        (gotMode == 2 ? center : gotMode == 1 ? fit : stretch).setChecked(true);
        /* listener AFTER the initial setChecked: opening the page must not
         * fire a write back to the daemon */
        scaleRg.setOnCheckedChangeListener((g, checkedId) ->
                WlBinder.configSet("scale_mode",
                        checkedId == 7 ? 2 : checkedId == 6 ? 1 : 0));
        root.addView(scaleRg);

        /* ---- XWayland scaling (daemon config xwayland_scale: X11 clients
         *      receive the zoom-adjusted X size on resize, then the existing
         *      stretch path scales them over the Android window) ---- */
        android.widget.Space gap26 = new android.widget.Space(this);
        gap26.setMinimumHeight(64);
        root.addView(gap26);

        TextView xwaylandScaleTip = new TextView(this);
        xwaylandScaleTip.setText(R.string.xwayland_scale_tip);
        root.addView(xwaylandScaleTip);

        android.widget.Switch xwaylandScaleSw = new android.widget.Switch(this);
        xwaylandScaleSw.setText(R.string.xwayland_scale);
        int gotXwaylandScale = WlBinder.configGet("xwayland_scale");
        xwaylandScaleSw.setChecked(gotXwaylandScale != 0); /* daemon default on */
        /* listener AFTER the initial state: opening the page must not fire a
         * write back to the daemon */
        xwaylandScaleSw.setOnCheckedChangeListener((b, on) ->
                WlBinder.configSet("xwayland_scale", on ? 1 : 0));
        root.addView(xwaylandScaleSw);

        /* ---- Auto attach (daemon config auto_attach: on = the daemon am-starts
         *      the host window the moment a wayland window is created; off =
         *      the window waits for a binder SURFACE from its app — the
         *      third-party path. Effective for windows created from now on.) */
        android.widget.Space gap24 = new android.widget.Space(this);
        gap24.setMinimumHeight(64);
        root.addView(gap24);

        TextView autoTip = new TextView(this);
        autoTip.setText(R.string.auto_attach_tip);
        root.addView(autoTip);

        android.widget.Switch autoSw = new android.widget.Switch(this);
        autoSw.setText(R.string.auto_attach);
        int gotAuto = WlBinder.configGet("auto_attach");
        autoSw.setChecked(gotAuto == 1);   /* daemon down / unknown → off */
        /* listener AFTER the initial state: opening the page must not fire a
         * write back to the daemon */
        autoSw.setOnCheckedChangeListener((b, on) -> WlBinder.configSet("auto_attach", on ? 1 : 0));
        root.addView(autoSw);

        /* ---- Initial window size (first-frame configure placeholder; #33,
         *      daemon config init_w/init_h — new windows only) ---- */
        android.widget.Space gap3 = new android.widget.Space(this);
        gap3.setMinimumHeight(64);
        root.addView(gap3);

        TextView sizeTip = new TextView(this);
        sizeTip.setText(R.string.init_size_tip);
        root.addView(sizeTip);

        final TextView sizeVal = new TextView(this);
        sizeVal.setTextSize(20);
        root.addView(sizeVal);

        /* pickers bounded to the daemon's accepted domain (a set outside it
         * is rejected — keep the UI from producing one) */
        final android.widget.NumberPicker wp = new android.widget.NumberPicker(this);
        final android.widget.NumberPicker hp = new android.widget.NumberPicker(this);
        android.widget.LinearLayout pickers = new android.widget.LinearLayout(this);
        pickers.setOrientation(android.widget.LinearLayout.HORIZONTAL);
        TextView wl = new TextView(this);
        wl.setText(R.string.init_size_w);
        android.widget.LinearLayout.LayoutParams plp =
                new android.widget.LinearLayout.LayoutParams(
                        0, android.widget.LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        android.widget.LinearLayout.LayoutParams tlp =
                new android.widget.LinearLayout.LayoutParams(
                        android.widget.LinearLayout.LayoutParams.WRAP_CONTENT,
                        android.widget.LinearLayout.LayoutParams.WRAP_CONTENT);
        pickers.addView(wl, tlp);
        pickers.addView(wp, plp);
        TextView hl = new TextView(this);
        hl.setText(R.string.init_size_h);
        pickers.addView(hl, tlp);
        pickers.addView(hp, plp);
        root.addView(pickers);

        int gw = WlBinder.configGet("init_w");
        int gh = WlBinder.configGet("init_h");
        final int[] sz = {(gw >= 100 && gw <= 7680) ? gw : 800,   /* daemon down → defaults */
                          (gh >= 100 && gh <= 4320) ? gh : 600};

        /* debounce like zoom (a scroll fires many changes); presets apply
         * immediately */
        final Runnable[] pendSz = new Runnable[1];
        final Runnable applySz = () -> {
            WlBinder.configSet("init_w", sz[0]);
            WlBinder.configSet("init_h", sz[1]);
        };
        final Runnable showSz = () -> sizeVal.setText(sz[0] + " × " + sz[1]);
        final Runnable changedSz = () -> {
            showSz.run();
            if (pendSz[0] != null) h.removeCallbacks(pendSz[0]);
            int w = sz[0], ht = sz[1];
            pendSz[0] = () -> {
                WlBinder.configSet("init_w", w);
                WlBinder.configSet("init_h", ht);
            };
            h.postDelayed(pendSz[0], 300);
        };
        android.widget.NumberPicker.OnValueChangeListener ncl = (p, o, n) -> {
            if (p == wp) sz[0] = n; else sz[1] = n;
            changedSz.run();
        };
        wp.setMinValue(100);
        wp.setMaxValue(7680);
        wp.setWrapSelectorWheel(false);
        wp.setValue(sz[0]);
        wp.setOnValueChangedListener(ncl);
        hp.setMinValue(100);
        hp.setMaxValue(4320);
        hp.setWrapSelectorWheel(false);
        hp.setValue(sz[1]);
        hp.setOnValueChangedListener(ncl);

        android.widget.LinearLayout sizes = new android.widget.LinearLayout(this);
        sizes.setOrientation(android.widget.LinearLayout.HORIZONTAL);
        int[][] presets2 = {{800, 600}, {1024, 768}, {1280, 720}, {1920, 1080}};
        for (int[] s : presets2) {
            android.widget.Button btn = new android.widget.Button(this);
            btn.setText(s[0] + "×" + s[1]);
            btn.setOnClickListener(v -> {
                if (pendSz[0] != null) h.removeCallbacks(pendSz[0]);
                pendSz[0] = null;
                sz[0] = s[0];
                sz[1] = s[1];
                wp.setValue(s[0]);
                hp.setValue(s[1]);
                showSz.run();
                h.post(applySz);
            });
            sizes.addView(btn, lp1());
        }
        root.addView(sizes);
        showSz.run();

        /* ---- Compositor backend (daemon config sc_enabled: on = the SC
         *      path — wayland layers become sibling ASurfaceControls
         *      composed by SurfaceFlinger/HWC, dma-buf scanout when
         *      allowed; off = the per-window GL renderer fallback.
         *      Windows already attached keep their backend until
         *      re-attach — effective for windows attached from now on.) */
        android.widget.Space gap4 = new android.widget.Space(this);
        gap4.setMinimumHeight(64);
        root.addView(gap4);

        TextView scTip = new TextView(this);
        scTip.setText(R.string.sc_backend_tip);
        root.addView(scTip);

        android.widget.Switch scSw = new android.widget.Switch(this);
        scSw.setText(R.string.sc_backend);
        int gotSc = WlBinder.configGet("sc_enabled");
        scSw.setChecked(gotSc != 0);   /* daemon down / unknown → on (the daemon default) */
        /* listener AFTER the initial state: opening the page must not fire a
         * write back to the daemon */
        scSw.setOnCheckedChangeListener((b, on) -> WlBinder.configSet("sc_enabled", on ? 1 : 0));
        root.addView(scSw);

        /* ---- VR (daemon config xr_*: the XR window geometry, the lens model
         *      and the title that selects it — docs/vr.md) ---- */
        section(R.string.xr_section);
        note(R.string.xr_section_tip);
        switchRow(R.string.xr_enabled, "xr_enabled", false);

        note(R.string.xr_mode_tip);
        radioRow("xr_mode", new int[]{8, 9, 10},
                new int[]{R.string.xr_mode_mono, R.string.xr_mode_sbs_h, R.string.xr_mode_sbs_v},
                new int[]{1, 2, 3}, 2);

        note(R.string.xr_refresh_tip);
        sliderRow(R.string.xr_refresh, "xr_refresh", 0, 144, 90, 1,
                v -> v == 0 ? getString(R.string.xr_refresh_panel) : v + " Hz");

        note(R.string.xr_eye_size_tip);
        eyeSizeRow();

        note(R.string.xr_fov_tip);
        sliderRow(R.string.xr_fov, "xr_fov", 30, 170, 96, 1, v -> v + "°");

        note(R.string.xr_ipd_tip);
        sliderRow(R.string.xr_ipd, "xr_ipd", 20, 90, 63, 1, v -> v + " mm");

        note(R.string.xr_scale_tip);
        sliderRow(R.string.xr_scale, "xr_scale", 200, 2000, 1000, 10, v -> (v / 10) + "%");

        note(R.string.xr_k1_tip);
        sliderRow(R.string.xr_k1, "xr_k1", -1000, 2000, 0, 10,
                v -> String.format(java.util.Locale.US, "%.3f", v / 1000.0));

        note(R.string.xr_k2_tip);
        sliderRow(R.string.xr_k2, "xr_k2", -1000, 2000, 0, 10,
                v -> String.format(java.util.Locale.US, "%.3f", v / 1000.0));

        note(R.string.xr_ca_tip);
        sliderRow(R.string.xr_ca, "xr_ca", 0, 100, 0, 1,
                v -> String.format(java.util.Locale.US, "%.2f", v / 1000.0));

        note(R.string.xr_predict_tip);
        sliderRow(R.string.xr_predict, "xr_predict", 0, 120, 0, 1,
                v -> v == 0 ? getString(R.string.xr_predict_off) : v + " ms");

        questPresetRow();

        note(R.string.xr_ext_pose_tip);

        textRow("xr_match", R.string.xr_match_tip);

        /* ---- Desktop (daemon config desktop_mode / desktop_match: run a
         *      nested compositor in one window — docs/desktop.md) ---- */
        section(R.string.desktop_section);
        note(R.string.desktop_section_tip);
        note(R.string.desktop_mode_tip);
        switchRow(R.string.desktop_mode, "desktop_mode", false);
        textRow("desktop_match", R.string.desktop_match_tip);
        note(R.string.desktop_hint);

        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        scroll.setFillViewport(true);
        scroll.addView(root);
        setContentView(scroll);
    }

    private static android.widget.LinearLayout.LayoutParams lp1() {
        return new android.widget.LinearLayout.LayoutParams(
                0, android.widget.LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
    }

    /* ---------------- daemon-config rows ----------------
     * Every row reads its initial state from the daemon (the single source of
     * truth, #31) and installs its listener AFTER the initial value, so merely
     * opening this page never writes anything back. */

    private void section(int titleRes) {
        android.widget.Space gap = new android.widget.Space(this);
        gap.setMinimumHeight(96);
        root.addView(gap);
        TextView t = new TextView(this);
        t.setText(titleRes);
        t.setTextSize(22);
        t.setTypeface(null, android.graphics.Typeface.BOLD);
        root.addView(t);
    }

    private void note(int tipRes) {
        android.widget.Space gap = new android.widget.Space(this);
        gap.setMinimumHeight(40);
        root.addView(gap);
        TextView t = new TextView(this);
        t.setText(tipRes);
        root.addView(t);
    }

    private android.widget.Switch switchRow(int labelRes, String key, boolean defOn) {
        android.widget.Switch sw = new android.widget.Switch(this);
        sw.setText(labelRes);
        int got = WlBinder.configGet(key);
        sw.setChecked(got < 0 ? defOn : got != 0);   /* daemon down → default */
        sw.setOnCheckedChangeListener((b, on) -> WlBinder.configSet(key, on ? 1 : 0));
        root.addView(sw);
        return sw;
    }

    private void radioRow(String key, int[] ids, int[] labelRes, int[] values, int defVal) {
        RadioGroup rg = new RadioGroup(this);
        RadioButton[] rbs = new RadioButton[ids.length];
        for (int i = 0; i < ids.length; i++) {
            rbs[i] = new RadioButton(this);
            rbs[i].setId(ids[i]);   /* unique across the whole activity */
            rbs[i].setText(labelRes[i]);
            rg.addView(rbs[i]);
        }
        int got = WlBinder.configGet(key);
        int cur = got < 0 ? defVal : got;
        for (int i = 0; i < values.length; i++)
            if (values[i] == cur) rbs[i].setChecked(true);
        rg.setOnCheckedChangeListener((g, checkedId) -> {
            for (int i = 0; i < ids.length; i++)
                if (ids[i] == checkedId) WlBinder.configSet(key, values[i]);
        });
        root.addView(rg);
    }

    /** daemon-backed slider with a step; 200ms drag debounce, immediate write
     *  on release (each write re-configures live windows / republishes) */
    private void sliderRow(int labelRes, String key, int lo, int hi, int defVal, int step,
                           java.util.function.IntFunction<String> fmt) {
        TextView tip = new TextView(this);
        tip.setText(labelRes);
        root.addView(tip);

        final TextView val = new TextView(this);
        val.setTextSize(20);
        root.addView(val);

        final android.widget.SeekBar sb = new android.widget.SeekBar(this);
        final int[] cur = {defVal};
        int got = WlBinder.configGet(key);
        if (got >= lo && got <= hi) cur[0] = lo + ((got - lo) / step) * step;
        sb.setMax((hi - lo) / step);
        sb.setProgress((cur[0] - lo) / step);
        root.addView(sb);
        val.setText(fmt.apply(cur[0]));

        final Runnable[] pend = new Runnable[1];
        sb.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(android.widget.SeekBar s, int prog, boolean fromUser) {
                if (!fromUser) return;
                cur[0] = lo + prog * step;
                val.setText(fmt.apply(cur[0]));
                if (pend[0] != null) h.removeCallbacks(pend[0]);
                int v = cur[0];
                pend[0] = () -> WlBinder.configSet(key, v);
                h.postDelayed(pend[0], 200);
            }
            @Override public void onStartTrackingTouch(android.widget.SeekBar s) { }
            @Override public void onStopTrackingTouch(android.widget.SeekBar s) {
                if (pend[0] != null) h.removeCallbacks(pend[0]);
                pend[0] = null;
                int v = cur[0];
                h.post(() -> WlBinder.configSet(key, v));
            }
        });
    }

    /** per-eye render target: two pickers (0 = derive from the eye layout and
     *  the panel), debounced like the initial-size row */
    private void eyeSizeRow() {
        final android.widget.NumberPicker wp = new android.widget.NumberPicker(this);
        final android.widget.NumberPicker hp = new android.widget.NumberPicker(this);
        android.widget.LinearLayout row = new android.widget.LinearLayout(this);
        row.setOrientation(android.widget.LinearLayout.HORIZONTAL);
        TextView wl = new TextView(this);
        wl.setText(R.string.xr_eye_w);
        TextView hl = new TextView(this);
        hl.setText(R.string.xr_eye_h);
        row.addView(wl);
        row.addView(wp, lp1());
        row.addView(hl);
        row.addView(hp, lp1());
        root.addView(row);

        int gw = WlBinder.configGet("xr_eye_w");
        int gh = WlBinder.configGet("xr_eye_h");
        final int[] sz = {(gw >= 0 && gw <= 4096) ? gw : 0,
                          (gh >= 0 && gh <= 4096) ? gh : 0};

        final Runnable[] pend = new Runnable[1];
        android.widget.NumberPicker.OnValueChangeListener ncl = (p, o, n) -> {
            if (p == wp) sz[0] = n; else sz[1] = n;
            if (pend[0] != null) h.removeCallbacks(pend[0]);
            int w = sz[0], ht = sz[1];
            pend[0] = () -> {
                WlBinder.configSet("xr_eye_w", w);
                WlBinder.configSet("xr_eye_h", ht);
            };
            h.postDelayed(pend[0], 300);
        };
        wp.setMinValue(0);
        wp.setMaxValue(4096);
        wp.setWrapSelectorWheel(false);
        wp.setValue(sz[0]);
        wp.setOnValueChangedListener(ncl);
        hp.setMinValue(0);
        hp.setMaxValue(4096);
        hp.setWrapSelectorWheel(false);
        hp.setValue(sz[1]);
        hp.setOnValueChangedListener(ncl);
    }

    /** string config (a title match list): TEXT + Apply, because the daemon
     *  writes config.json unescaped and rejects quotes / backslashes */
    private void textRow(String key, int tipRes) {
        TextView tip = new TextView(this);
        tip.setText(tipRes);
        root.addView(tip);

        final android.widget.EditText ed = new android.widget.EditText(this);
        ed.setSingleLine(true);
        String cur = WlBinder.configGetStr(key);
        if (cur != null) ed.setText(cur);
        root.addView(ed);

        final TextView status = new TextView(this);
        status.setTextSize(14);
        root.addView(status);

        android.widget.Button apply = new android.widget.Button(this);
        apply.setText(R.string.cfg_str_apply);
        apply.setOnClickListener(v -> {
            if (!WlBinder.available()) {
                status.setText(R.string.cfg_str_nodaemon);
                return;
            }
            int rc = WlBinder.configSetStr(key, ed.getText().toString().trim());
            status.setText(rc == 0 ? "" : getString(R.string.cfg_str_rejected));
        });
        root.addView(apply);
    }

    /** one button that fills in the numbers for a Quest 3S (1832x1920/eye,
     *  90 Hz, ~96° per eye, 63 mm IPD) — the distortion terms stay a starting
     *  point that has to be tuned through the lenses */
    private void questPresetRow() {
        android.widget.Button b = new android.widget.Button(this);
        b.setText(R.string.xr_preset_quest);
        b.setOnClickListener(v -> {
            WlBinder.configSet("xr_mode", 2);
            WlBinder.configSet("xr_refresh", 90);
            WlBinder.configSet("xr_eye_w", 1832);
            WlBinder.configSet("xr_eye_h", 1920);
            WlBinder.configSet("xr_fov", 96);
            WlBinder.configSet("xr_ipd", 63);
            WlBinder.configSet("xr_scale", 900);
            WlBinder.configSet("xr_k1", 220);
            WlBinder.configSet("xr_k2", 40);
            WlBinder.configSet("xr_ca", 10);
            WlBinder.configSet("xr_predict", 20);
            recreate();   /* rebuild the page from the daemon's new values */
        });
        root.addView(b);
    }
}
