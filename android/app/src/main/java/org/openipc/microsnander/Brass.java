/*
 * Copyright (c) OpenIPC  https://openipc.org  MIT License
 *
 * Brass.java — the brass look built in code (brand brass 1.3.0): a row of the
 * lamp board, the band of a settings section, a brass dialog with its panel,
 * title and buttons. Colours come from res/values/colors.xml.
 */
package org.openipc.microsnander;

import android.app.Activity;
import android.app.Dialog;
import android.content.Context;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.util.DisplayMetrics;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

final class Brass {
    private Brass() {
    }

    @SuppressWarnings("deprecation")   // getColor(int) of Resources: the only one before 6.0
    static int color(Context c, int id) {
        return Build.VERSION.SDK_INT >= 23 ? c.getColor(id) : c.getResources().getColor(id);
    }

    static int dp(Context c, float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v,
                c.getResources().getDisplayMetrics());
    }

    /** A lamp: a circle in the state colour, the thin #6e5320 rim of the web page's lamps. */
    static GradientDrawable lamp(Context c, int colorId) {
        GradientDrawable d = new GradientDrawable();
        d.setShape(GradientDrawable.OVAL);
        d.setColor(color(c, colorId));
        d.setStroke(dp(c, 1), 0xff6e5320);
        return d;
    }

    /** One row of the lamp board: lamp (12 dp), label (dim mono caps), value (ink mono). */
    static final class Row {
        final LinearLayout view;
        final View lamp;
        final TextView label, value;

        Row(Context c, int labelId) {
            view = new LinearLayout(c);
            view.setOrientation(LinearLayout.HORIZONTAL);
            view.setGravity(Gravity.CENTER_VERTICAL);
            view.setPadding(0, dp(c, 3), 0, dp(c, 3));
            lamp = new View(c);
            int s = dp(c, 12);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(s, s);
            lp.setMargins(0, 0, dp(c, 10), 0);
            view.addView(lamp, lp);
            label = new TextView(c);
            label.setText(labelId);
            label.setTypeface(Typeface.MONOSPACE);
            label.setTextSize(13);
            label.setLetterSpacing(0.08f);
            label.setTextColor(color(c, R.color.dim));
            view.addView(label, new LinearLayout.LayoutParams(dp(c, 84), LinearLayout.LayoutParams.WRAP_CONTENT));
            value = new TextView(c);
            value.setTypeface(Typeface.MONOSPACE);
            value.setTextSize(13);
            value.setTextColor(color(c, R.color.ink));
            value.setCompoundDrawablePadding(dp(c, 10));
            view.addView(value, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        }

        /** colorId 0: no lamp (the empty place keeps the column); icon 0: none after the value */
        void set(int colorId, CharSequence v, int icon) {
            lamp.setBackground(colorId == 0 ? null : Brass.lamp(view.getContext(), colorId));
            value.setText(v);
            value.setCompoundDrawablesWithIntrinsicBounds(0, 0, icon, 0);
        }

        void set(int colorId, CharSequence v) {
            set(colorId, v, 0);
        }
    }

    /** A section of a dialog: a full-width brass plate band with its name, dark serif caps. */
    static void section(LinearLayout box, Context c, CharSequence text, boolean gap) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setAllCaps(true);
        t.setTypeface(Typeface.SERIF, Typeface.BOLD);
        t.setLetterSpacing(0.25f);
        t.setTextSize(15);
        t.setTextColor(color(c, R.color.ink_dark));
        t.setBackgroundResource(R.drawable.plate);
        t.setGravity(Gravity.CENTER);
        int p = dp(c, 8);
        t.setPadding(p, p, p, p);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(0, dp(c, gap ? 28 : 8), 0, dp(c, 10));
        box.addView(t, lp);
    }

    /** The heading of a dialog: brass serif capitals with a soft glow, centred. */
    static TextView title(Context c, int text) {
        TextView t = new TextView(c);
        t.setText(text);
        t.setAllCaps(true);
        t.setTextSize(20);
        t.setTypeface(Typeface.SERIF);
        t.setLetterSpacing(0.2f);
        t.setTextColor(color(c, R.color.brass));
        t.setShadowLayer(dp(c, 8), 0, 0, 0x59c9a24d);
        t.setGravity(Gravity.CENTER_HORIZONTAL);
        return t;
    }

    /** The panel of a dialog: a vertical column on the brass2-framed gradient. */
    static LinearLayout panel(Context c) {
        LinearLayout p = new LinearLayout(c);
        p.setOrientation(LinearLayout.VERTICAL);
        p.setBackgroundResource(R.drawable.panel);
        int pad = dp(c, 18);
        p.setPadding(pad, pad, pad, pad);
        return p;
    }

    static Button button(Context c, int text) {
        Button b = new Button(c, null, 0, R.style.Brass);
        b.setText(text);
        return b;
    }

    /** Layout parameters with the 6 dp margin of a brass button. */
    static LinearLayout.LayoutParams margins(Context c, int width, int height, float weight) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(width, height, weight);
        int m = dp(c, 6);
        lp.setMargins(m, m, m, m);
        return lp;
    }

    /**
     * A brass dialog of frame: no system frame or title, at most maxWidthDp wide (94 % of a phone);
     * fill: its height, a share of the screen, or 0 for as high as its content.
     */
    static Dialog dialog(Activity a, View frame, int maxWidthDp, float fill) {
        Dialog d = new Dialog(a, android.R.style.Theme_Material_Dialog_NoActionBar);
        d.setContentView(frame);
        if (d.getWindow() != null) {
            d.getWindow().setBackgroundDrawable(new ColorDrawable(0));
            DisplayMetrics dm = a.getResources().getDisplayMetrics();
            d.getWindow().setLayout(Math.min((int) (dm.widthPixels * 0.94), dp(a, maxWidthDp)),
                    fill > 0 ? (int) (dm.heightPixels * fill) : LinearLayout.LayoutParams.WRAP_CONTENT);
        }
        return d;
    }
}
