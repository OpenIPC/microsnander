plugins {
    id("com.android.application") version "8.2.0" apply false
    id("com.android.library") version "8.2.0" apply false   // :camex-tunnel
}

// The ABIs of the app (-PmicrosnanderAbis, app/build.gradle.kts checks them) for the
// native code of :camex-tunnel too
extra["camexTunnelAbis"] = ((findProperty("microsnanderAbis") as String?) ?: "arm64-v8a,armeabi-v7a")
    .split(",").map { it.trim() }
