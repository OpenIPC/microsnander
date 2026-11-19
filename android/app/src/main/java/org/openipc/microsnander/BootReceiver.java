package org.openipc.microsnander;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/**
 * Remote access with nobody at the device. After a reboot: when it is on and Start on boot is
 * checked. After an update of the app (Android stops the app to replace it): when the page was
 * serving before, so a box with no screen comes back by itself, its tunnel too.
 */
public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context c, Intent intent) {
        String a = intent.getAction();
        if (Intent.ACTION_BOOT_COMPLETED.equals(a)) {
            if (Settings.active(c) && Settings.startOnBoot(c))
                RemoteService.send(c, RemoteService.ACTION_START);
            else
                Settings.setServing(c, false);	// not serving since this boot
        } else if (Intent.ACTION_MY_PACKAGE_REPLACED.equals(a) && Settings.active(c) && Settings.serving(c)) {
            AppLog.add("app", "updated to " + BuildConfig.VERSION_NAME + ": remote access starts again");
            RemoteService.send(c, RemoteService.ACTION_START);
        }
    }
}
