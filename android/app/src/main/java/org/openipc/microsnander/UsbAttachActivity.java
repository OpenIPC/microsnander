package org.openipc.microsnander;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/**
 * Started by Android when a programmer is plugged in (res/xml/programmers.xml).
 * Choosing the app there with "use by default" (Always) grants it the
 * programmer on every later plug-in and after a reboot without asking, so
 * remote access needs nobody at the device. With remote access on it only
 * makes sure the service runs; otherwise it opens the app, as before.
 */
public class UsbAttachActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (Settings.active(this))
            RemoteService.send(this, RemoteService.ACTION_START);
        else
            startActivity(new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        finish();
    }
}
