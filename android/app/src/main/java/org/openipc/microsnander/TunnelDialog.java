package org.openipc.microsnander;

import android.app.Activity;
import android.app.AlertDialog;
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
 * The settings of the camex tunnel: on or off, then the fields of the tunnel
 * library (server, client name, UDP or TCP, the server's public key, the psk,
 * optional MTU and keepalive). A link with the settings (the one camex's
 * install.sh prints) fills them: pasted here, or opened with the app.
 */
final class TunnelDialog {
    interface Saved { void saved(boolean on); }

    private TunnelDialog() {
    }

    /** the dialog over the saved settings, with the values of link (or null) over them */
    static void show(Activity a, String link, Saved done) {
        Map<String, String> v = Settings.tunnel(a);
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (20 * a.getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, 0);

        CheckBox on = new CheckBox(a);
        on.setText(R.string.tunnel_allow);
        on.setChecked(Settings.tunnelEnabled(a));
        box.addView(on);

        TextView note = new TextView(a);
        note.setText(R.string.tunnel_note);
        note.setTextSize(12);
        box.addView(note);

        // the link: pasted and Apply, or the one the app was opened with
        EditText linkField = new EditText(a);
        linkField.setHint(R.string.tunnel_link_hint);
        linkField.setSingleLine(true);
        linkField.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        linkField.setImeOptions(EditorInfo.IME_ACTION_DONE);
        box.addView(linkField);
        Button apply = new Button(a);
        apply.setText(R.string.tunnel_link_apply);
        box.addView(apply);
        TextView linkErrors = new TextView(a);
        linkErrors.setTextColor(0xffc8452c);	// R.color.red; getColor(id, theme) is Android 6+
        linkErrors.setVisibility(View.GONE);
        box.addView(linkErrors);

        Map<String, EditText> texts = new LinkedHashMap<>();
        Map<String, RadioGroup> choices = new HashMap<>();
        for (Field f : CamexTunnel.fields()) {
            TextView label = new TextView(a);
            label.setText(f.required ? f.label : a.getString(R.string.tunnel_optional, f.label));
            label.setPadding(0, pad / 2, 0, 0);
            box.addView(label);
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
            } else if (f.kind != Field.Kind.FLAG) {
                EditText e = new EditText(a);
                e.setSingleLine(true);
                // shown, not dotted: typed with a TV remote, read back before Save
                e.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
                e.setText(value == null ? "" : value);
                texts.put(f.key, e);
                box.addView(e);
            }
        }
        if (link != null)
            fill(a, link, texts, choices, linkErrors);
        apply.setOnClickListener(b -> fill(a, linkField.getText().toString().trim(), texts, choices, linkErrors));

        ScrollView sv = new ScrollView(a);
        sv.addView(box);
        AlertDialog d = new AlertDialog.Builder(a).setTitle(R.string.tunnel_title).setView(sv)
                .setPositiveButton(R.string.tunnel_save, null)
                .setNegativeButton(android.R.string.cancel, null).show();
        // Save checks the fields first: the dialog stays with what is wrong
        d.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(b -> {
            Map<String, String> nv = new HashMap<>();
            for (Map.Entry<String, EditText> e : texts.entrySet())
                nv.put(e.getKey(), e.getValue().getText().toString().trim());
            for (Map.Entry<String, RadioGroup> e : choices.entrySet()) {
                View r = e.getValue().findViewById(e.getValue().getCheckedRadioButtonId());
                nv.put(e.getKey(), r instanceof RadioButton ? ((RadioButton) r).getText().toString() : "");
            }
            if (on.isChecked()) {
                String[] bad = CamexTunnel.check(nv);
                if (bad != null) {
                    EditText e = texts.get(bad[0]);
                    if (e != null) {
                        e.setError(bad[1]);
                        e.requestFocus();
                    } else
                        linkErrors.setText(bad[1]);
                    return;
                }
            }
            Settings.saveTunnel(a, on.isChecked(), nv);
            d.dismiss();
            done.saved(on.isChecked());
        });
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
