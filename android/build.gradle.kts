buildscript {
    dependencies {
        // AGP owns Kotlin integration; upgrade its runtime KGP without kotlin-android.
        classpath(libs.kotlin.gradle.plugin)
    }
}

plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.compose) apply false
}

val localBuildRoot = providers.environmentVariable("OKUFLOW_ANDROID_BUILD_DIR")
    .orElse(providers.environmentVariable("LOCALAPPDATA").map { "$it/OkuFlow/android-build" })
    .orElse(providers.systemProperty("user.home").map { "$it/.cache/okuflow/android-build" })
    .get()

allprojects {
    layout.buildDirectory.set(java.io.File(localBuildRoot, name))
}
