# The native code calls Progress.progress() and builds Native.Result by name.
-keep class org.openipc.microsnander.Native { *; }
-keep class org.openipc.microsnander.Native$* { *; }
# ... and asks RemoteService for the programmer of the page's operations (JNI_OnLoad).
-keep class org.openipc.microsnander.RemoteService {
    static long openDevice();
    static void closeDevice();
}
