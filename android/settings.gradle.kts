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
