plugins {
    id("com.android.application")
}

// The version lives in _VER in src/main.c only: versionName is that,
// versionCode MAJOR * 10000 + MINOR * 100 + PATCH.
val msVersion: String = run {
    val main = rootProject.file("../src/main.c")
    val m = Regex("""(?m)^#define\s+_VER\s+"(\d+)\.(\d+)\.(\d+)"""")
        .find(main.readText())
        ?: throw GradleException("no _VER \"X.Y.Z\" in $main")
    m.groupValues.drop(1).joinToString(".")
}
val msVersionCode: Int = msVersion.split(".").map { it.toInt() }
    .let { (major, minor, patch) -> major * 10000 + minor * 100 + patch }

// Release keystore from the environment (CI secret); without it the release stays unsigned.
val releaseKeystore: String? =
    System.getenv("MICROSNANDER_KEYSTORE_FILE")?.takeIf { it.isNotBlank() }

android {
    namespace = "org.openipc.microsnander"
    compileSdk = 34
    ndkVersion = "27.2.12479018"   // NDK r27c, as in compot/android and CI

    defaultConfig {
        applicationId = "org.openipc.microsnander"
        minSdk = 21
        targetSdk = 34
        versionCode = msVersionCode
        versionName = msVersion
        // phones and TV boxes (32-bit ARM too) (#89). -PmicrosnanderAbis=armeabi-v7a
        // for one ABI only, add x86_64 for an x86_64 emulator.
        // An empty list would leave abiFilters unset, i.e. every ABI of the NDK; an empty
        // entry ("a,,b", "a,") is an error too (#92)
        val abis = ((project.findProperty("microsnanderAbis") as String?) ?: "arm64-v8a,armeabi-v7a")
            .split(",").map { it.trim() }
        val knownAbis = setOf("arm64-v8a", "armeabi-v7a", "x86_64", "x86")
        require(abis.isNotEmpty() && abis.all { it in knownAbis }) {
            "-PmicrosnanderAbis=${abis.joinToString(",")}: one or more of ${knownAbis.joinToString(",")}"
        }
        ndk { abiFilters += abis }
        externalNativeBuild { cmake { arguments += "-DMS_VERSION=$msVersion" } }
        resourceConfigurations += listOf("en", "ru", "zh-rCN")
    }
    signingConfigs {
        // A fixed debug key in the repository (not a secret, as every debug
        // key): the CI debug APKs install over each other.
        getByName("debug") {
            storeFile = rootProject.file("debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
        create("release") {
            if (releaseKeystore != null) {
                storeFile = file(releaseKeystore)
                storePassword = System.getenv("MICROSNANDER_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("MICROSNANDER_KEY_ALIAS")
                keyPassword = System.getenv("MICROSNANDER_KEY_PASSWORD")
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            if (releaseKeystore != null) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }
    buildFeatures { buildConfig = true }
    externalNativeBuild {
        cmake { path("src/main/cpp/CMakeLists.txt") }
    }
    lint {
        warningsAsErrors = true
        abortOnError = true
        // GradleDependency etc.: the versions are pinned on purpose (as in compot/android);
        // ChromeOsAbiSupport: phones and TV boxes, no x86 by default
        disable += listOf("GradleDependency", "OldTargetApi", "AndroidGradlePluginVersion", "ChromeOsAbiSupport")
    }
}

dependencies {
    implementation(project(":camex-tunnel"))
}
