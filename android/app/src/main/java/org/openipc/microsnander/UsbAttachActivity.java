package org.openipc.microsnander;

import android.app.Activity;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.os.UserManager;

/**
 * Started by Android when a programmer is plugged in (res/xml/programmers.xml).
 * Choosing the app there with "use by default" (Always) grants it the
 * programmer on every later plug-in and after a reboot without asking, so
 * remote access needs nobody at the device. With remote access on it only
 * makes sure the service runs; otherwise it opens the app, as before.
 *
 * A programmer that stays plugged in is found early in a boot, before the
 * user's storage is unlocked: Android then sees only direct-boot-aware
 * components, so this one is, or the default would not be found and the
 * programmer would not be granted. At that time it only finishes (the
 * settings are not readable yet); the service comes with BOOT_COMPLETED.
 */
public class UsbAttachActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (Build.VERSION.SDK_INT >= 24 && !getSystemService(UserManager.class).isUserUnlocked()) {
            finish();		// granted already by Android, which started this
            return;
        }
        if (Settings.active(this))
            RemoteService.send(this, RemoteService.ACTION_START);
        else
            startActivity(new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        finish();
    }
}
