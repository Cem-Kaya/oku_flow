pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "OkuFlowCamera"
include(":app")

// Also applies during IDE sync. The build script supplies the same property via -P.
val localBuildRoot = providers.environmentVariable("OKUFLOW_ANDROID_BUILD_DIR")
    .orElse(providers.environmentVariable("LOCALAPPDATA").map { "$it/OkuFlow/android-build" })
    .orElse(providers.systemProperty("user.home").map { "$it/.cache/okuflow/android-build" })
    .get()
require(java.io.File(localBuildRoot).isAbsolute) {
    "OKUFLOW_ANDROID_BUILD_DIR must be an absolute path outside the synced checkout."
}
val outputDirectory = java.io.File(localBuildRoot).canonicalFile
require(!outputDirectory.toPath().startsWith(rootDir.parentFile.canonicalFile.toPath()) &&
    !outputDirectory.path.startsWith("Y:", ignoreCase = true)) {
    "Android build outputs must stay outside the Google Drive checkout."
}
gradle.beforeProject {
    extensions.extraProperties["kotlin.project.persistent.dir"] =
        java.io.File(localBuildRoot, "kotlin-persistent").absolutePath
}
