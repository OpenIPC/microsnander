// microsnander Android app: the same operations as the web mode, on a
// programmer plugged into the phone or TV box (USB host).
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "microsnander"
include(":app")
// camex built in (remote access over a camex server): the tunnel library of the
// camex sources, a submodule pinned to a camex release
include(":camex-tunnel")
project(":camex-tunnel").projectDir = file("../bundles/camex/android/tunnel")
