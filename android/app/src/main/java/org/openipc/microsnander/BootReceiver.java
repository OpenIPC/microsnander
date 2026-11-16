package org.openipc.microsnander;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Remote access after a reboot, with nobody at the device: when it is on and has a password. */
public class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context c, Intent intent) {
        if (Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction()) && Settings.active(c))
            RemoteService.send(c, RemoteService.ACTION_START);
    }
}
