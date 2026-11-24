plugins { id("com.android.application") }

android {
    namespace = "ch.plum.mewgenics"
    compileSdk = 35
    ndkVersion = "27.3.13750724"   // the NDK tools/build_android_so.sh uses
    buildToolsVersion = "35.0.0"
    defaultConfig {
        applicationId = "ch.plum.mewgenics"
        minSdk = 29
        targetSdk = 35
        versionCode = 4
        versionName = "0.2.2"
        ndk { abiFilters += "arm64-v8a" }
    }
    // libmain.so (runtime + lifted game code) and libSDL3.so are prebuilt with the NDK from the
    // player's own Mewgenics.exe by tools/build.sh (tools/build_android_so.sh).
    packaging { jniLibs { useLegacyPackaging = true } }
    // Release signing: your own key via MEW_KEYSTORE (+ _PASSWORD, MEW_KEY_ALIAS, MEW_KEY_PASSWORD),
    // otherwise the SDK debug key, which is fine for sideloading.
    val ks = System.getenv("MEW_KEYSTORE")
    signingConfigs {
        if (ks != null) create("own") {
            storeFile = file(ks)
            storePassword = System.getenv("MEW_KEYSTORE_PASSWORD")
            keyAlias = System.getenv("MEW_KEY_ALIAS")
            keyPassword = System.getenv("MEW_KEY_PASSWORD")
        }
    }
    buildTypes {
        getByName("debug") { isDebuggable = true }
        getByName("release") {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName(if (ks != null) "own" else "debug")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
