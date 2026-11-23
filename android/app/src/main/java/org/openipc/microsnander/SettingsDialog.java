package org.openipc.microsnander;

import android.app.Activity;
import android.app.Dialog;
import android.text.InputType;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;
import org.openipc.camex.tunnel.CamexTunnel;
import org.openipc.camex.tunnel.ConnectLink;
import org.openipc.camex.tunnel.Field;

/**
 * The one brass SETTINGS window (brass 1.3.0, as vectis): the app's section
 * (Allow remote access, the port, the web page password) and CAMEX (Join a
 * camex server, then the fields of the tunnel library as it gives them). A
 * camex link the app is opened with (the one camex's install.sh prints) fills
 * the CAMEX fields; what is malformed in it is listed.
 */
final class SettingsDialog {
    /** Save: what was on before, and whether the page's port or password changed */
    interface Saved { void saved(boolean wasActive, boolean pageChanged); }

    private SettingsDialog() {
    }

    /** the window over the saved settings, with the values of link (or null) over them */
    static void show(Activity a, String link, Saved done) {
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(Brass.dp(a, 4), 0, Brass.dp(a, 4), 0);

        Brass.section(box, a, a.getString(R.string.app_name), false);
        CheckBox remote = new CheckBox(a);
        remote.setText(R.string.remote_allow);
        remote.setChecked(Settings.enabled(a));
        box.addView(remote);
        label(a, box, a.getString(R.string.remote_port));
        EditText port = field(a, box, String.valueOf(Settings.port(a)), null);
        port.setInputType(InputType.TYPE_CLASS_NUMBER);
        port.setTypeface(Brass.mono(a));      // a new input type puts the system's face back
        label(a, box, a.getString(R.string.remote_password));
        // shown, not dotted: typed with a TV remote, read back before Save
        EditText password = field(a, box, Settings.password(a), a.getString(R.string.remote_password_hint));

        Brass.section(box, a, "Camex", true);
        CheckBox join = new CheckBox(a);
        join.setText(R.string.tunnel_allow);
        join.setChecked(Settings.tunnelEnabled(a));
        box.addView(join);
        TextView linkErrors = new TextView(a);
        linkErrors.setTextColor(Brass.color(a, R.color.red));
        linkErrors.setTypeface(Brass.mono(a));
        linkErrors.setVisibility(View.GONE);
        box.addView(linkErrors);

        Map<String, String> v = Settings.tunnel(a);
        Map<String, EditText> texts = new LinkedHashMap<>();
        Map<String, RadioGroup> choices = new HashMap<>();
        // the fields as the library gives them: order, labels, hints, the client name of a new
        // setting (microsnander-<the last 4 of ANDROID_ID>), the key hidden with Show the key
        for (Field f : CamexTunnel.fields(a, Settings.APP)) {
            if (CamexTunnel.WEB_PORT.equals(f.key) || f.kind == Field.Kind.FLAG)
                continue;		// the web port is the page's, from the section above
            label(a, box, f.label);
            String value = v.containsKey(f.key) && !v.get(f.key).isEmpty() ? v.get(f.key) : f.initial;
            if (f.kind == Field.Kind.CHOICE) {
                RadioGroup g = new RadioGroup(a);
                g.setOrientation(RadioGroup.HORIZONTAL);
                for (String c : f.choices) {
                    RadioButton r = new RadioButton(a);
                    r.setText(c);
                    r.setId(View.generateViewId());
                    g.addView(r);
                    if (c.equals(value)) g.check(r.getId());
                }
                choices.put(f.key, g);
                box.addView(g);
            } else {
                EditText e = field(a, box, value, f.hint.isEmpty() ? null : f.hint);
                texts.put(f.key, e);
                if (f.kind == Field.Kind.SECRET) {
                    int hidden = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD;
                    e.setInputType(hidden);
                    e.setTypeface(Brass.mono(a));
                    CheckBox show = new CheckBox(a);
                    show.setText(Field.SHOW_SECRET);
                    show.setOnCheckedChangeListener((b, on) -> {
                        e.setInputType(on ? InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
                                : hidden);
                        e.setTypeface(Brass.mono(a));
                        e.setSelection(e.getText().length());
                    });
                    box.addView(show);
                }
            }
        }
        if (link != null) {
            join.setChecked(true);
            fill(a, link, texts, choices, linkErrors);
        }

        LinearLayout frame = Brass.panel(a);
        frame.addView(Brass.title(a, R.string.settings));
        ScrollView sv = new ScrollView(a);
        sv.addView(box);
        LinearLayout.LayoutParams sl = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1);
        sl.setMargins(0, Brass.dp(a, 8), 0, Brass.dp(a, 8));
        frame.addView(sv, sl);
        LinearLayout actions = new LinearLayout(a);
        Button cancel = Brass.button(a, android.R.string.cancel);
        Button save = Brass.button(a, R.string.tunnel_save);
        actions.addView(cancel, Brass.margins(a, 0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        actions.addView(save, Brass.margins(a, 0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        frame.addView(actions);
        Dialog d = Brass.dialog(a, frame, 640, 0.9f);
        cancel.setOnClickListener(x -> d.dismiss());
        // Save checks the fields first: the window stays with what is wrong
        save.setOnClickListener(x -> {
            int p;
            try {
                p = Integer.parseInt(port.getText().toString().trim());
            } catch (NumberFormatException e) {
                p = 0;
            }
            if (p < 1024 || p > 65535) {
                port.setError(a.getString(R.string.remote_bad_port));
                port.requestFocus();
                return;
            }
            Map<String, String> nv = new HashMap<>();
            for (Map.Entry<String, EditText> e : texts.entrySet())
                nv.put(e.getKey(), e.getValue().getText().toString().trim());
            for (Map.Entry<String, RadioGroup> e : choices.entrySet()) {
                View r = e.getValue().findViewById(e.getValue().getCheckedRadioButtonId());
                nv.put(e.getKey(), r instanceof RadioButton ? ((RadioButton) r).getText().toString() : "");
            }
            if (join.isChecked()) {
                String[] bad = CamexTunnel.check(nv);
                if (bad != null) {
                    EditText e = texts.get(bad[0]);
                    if (e != null) {
                        e.setError(bad[1]);
                        e.requestFocus();
                    } else {
                        linkErrors.setText(bad[1]);
                        linkErrors.setVisibility(View.VISIBLE);
                    }
                    return;
                }
            }
            String pw = password.getText().toString();
            boolean was = Settings.active(a);
            boolean changed = p != Settings.port(a) || !pw.equals(Settings.password(a));
            Settings.save(a, remote.isChecked(), p, pw);
            Settings.saveTunnel(a, join.isChecked(), nv);
            d.dismiss();
            done.saved(was, changed);
        });
        d.show();
    }

    private static void label(Activity a, LinearLayout box, String text) {
        TextView t = new TextView(a);
        t.setText(text);
        t.setTypeface(Brass.mono(a));
        t.setTextColor(Brass.color(a, R.color.dim));
        t.setPadding(0, Brass.dp(a, 10), 0, 0);
        box.addView(t);
    }

    private static EditText field(Activity a, LinearLayout box, String value, String hint) {
        EditText e = new EditText(a);
        e.setSingleLine(true);
        e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
        e.setImeOptions(EditorInfo.IME_ACTION_NEXT);
        e.setTypeface(Brass.mono(a));         // after the input type: it sets a face of its own
        e.setText(value == null ? "" : value);
        if (hint != null) e.setHint(hint);
        box.addView(e);
        return e;
    }

    /** the values of a camex link into the fields; its malformed fields are listed */
    private static void fill(Activity a, String link, Map<String, EditText> texts, Map<String, RadioGroup> choices,
                             TextView errors) {
        ConnectLink l = ConnectLink.parse(link);
        if (l == null) {
            errors.setText(R.string.tunnel_link_bad);
            errors.setVisibility(View.VISIBLE);
            return;
        }
        for (Map.Entry<String, String> e : l.values.entrySet()) {
            EditText t = texts.get(e.getKey());
            if (t != null)
                t.setText(e.getValue());
            RadioGroup g = choices.get(e.getKey());
            for (int i = 0; g != null && i < g.getChildCount(); i++) {
                RadioButton r = (RadioButton) g.getChildAt(i);
                if (r.getText().toString().equalsIgnoreCase(e.getValue()))
                    g.check(r.getId());
            }
        }
        StringBuilder s = new StringBuilder();
        for (Map.Entry<String, String> e : l.errors.entrySet())
            s.append(a.getString(R.string.tunnel_link_field, e.getKey(), e.getValue())).append('\n');
        errors.setText(s.toString().trim());
        errors.setVisibility(s.length() > 0 ? View.VISIBLE : View.GONE);
    }
}
