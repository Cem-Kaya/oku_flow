import java.io.File
import java.security.MessageDigest

plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// Secrets are process inputs only. Never use the debug key for a release artifact.
val uploadVariables = listOf("OKUFLOW_ANDROID_UPLOAD_KEYSTORE", "OKUFLOW_ANDROID_UPLOAD_KEY_ALIAS",
    "OKUFLOW_ANDROID_UPLOAD_STORE_PASSWORD", "OKUFLOW_ANDROID_UPLOAD_KEY_PASSWORD")
val uploadInputs = uploadVariables.map { providers.environmentVariable(it).orNull }
val uploadConfigured = uploadInputs.all { !it.isNullOrBlank() }
require(uploadInputs.all { it.isNullOrBlank() } || uploadConfigured) {
    "Android upload signing requires all four OKUFLOW_ANDROID_UPLOAD_* inputs; partial configuration is refused."
}
val allowUnsignedRelease = providers.gradleProperty("okuflow.allowUnsignedRelease").orNull == "true"
require(!allowUnsignedRelease || !uploadConfigured) {
    "Unsigned review mode requires all upload-signing inputs to be cleared."
}
if (uploadConfigured) {
    val keyPath = File(uploadInputs[0]!!)
    require(keyPath.isAbsolute && keyPath.isFile && !keyPath.toPath().normalize().startsWith(rootDir.toPath()) &&
        !keyPath.absolutePath.startsWith("Y:", ignoreCase = true)) {
        "The upload keystore must be an existing absolute local file outside the checkout and Drive."
    }
}

android {
    namespace = "com.okuflow.camera"
    compileSdk {
        version = release(37) { minorApiLevel = 2 }
    }
    buildToolsVersion = "37.0.0"
    ndkVersion = "30.0.16248370"

    defaultConfig {
        // Play's immutable package; Kotlin classes/resource namespace stay com.okuflow.camera.
        applicationId = "com.okuflow"
        minSdk = 30
        targetSdk = 37
        versionCode = 1
        versionName = "0.1.0"
    }

    signingConfigs {
        if (uploadConfigured) create("upload") {
            storeFile = File(uploadInputs[0]!!)
            keyAlias = uploadInputs[1]
            storePassword = uploadInputs[2]
            keyPassword = uploadInputs[3]
        }
    }
    buildTypes {
        getByName("release") {
            isDebuggable = false
            // Minification is a separate measured optimization, not a Play publication prerequisite.
            isMinifyEnabled = false
            if (uploadConfigured) signingConfig = signingConfigs.getByName("upload")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    buildFeatures {
        compose = true
    }
    sourceSets {
        named("main") { kotlin.directories.add("src/main/java") }
        named("test") { kotlin.directories.add("src/test/java") }
    }
}

gradle.taskGraph.whenReady {
    if (allTasks.any { it.project == project && it.name.endsWith("Release") } &&
        !uploadConfigured && !allowUnsignedRelease) {
        throw GradleException("Release signing inputs are missing. Supply the upload key inputs or explicitly use -Pokuflow.allowUnsignedRelease=true for a non-uploadable review artifact.")
    }
}

tasks.register("writeReleaseDependencyInventory") {
    val inventoryFile = layout.buildDirectory.file("reports/release/runtime-dependencies.json")
    outputs.file(inventoryFile)
    // Always resolve the current graph; it must not silently reuse a stale review inventory.
    outputs.upToDateWhen { false }
    doLast {
        val artifacts = configurations.getByName("releaseRuntimeClasspath").resolvedConfiguration.resolvedArtifacts
            .sortedBy { "${it.moduleVersion.id}:${it.name}:${it.classifier}" }.map { artifact ->
                val digest = MessageDigest.getInstance("SHA-256")
                artifact.file.inputStream().use { input ->
                    val buffer = ByteArray(65536)
                    while (true) { val count = input.read(buffer); if (count < 0) break; digest.update(buffer, 0, count) }
                }
                mapOf("module" to artifact.moduleVersion.id.toString(), "artifact" to artifact.file.name,
                    "sha256" to digest.digest().joinToString("") { "%02x".format(it.toInt() and 0xff) })
            }
        val output = inventoryFile.get().asFile
        output.parentFile.mkdirs()
        val reviewFile = file("src/main/assets/licenses/runtime-review.json")
        val review = groovy.json.JsonSlurper().parse(reviewFile) as Map<*, *>
        val reviewed = review["components"] as List<*>
        fun artifactKey(item: Map<*, *>) = listOf(item["module"], item["artifact"], item["sha256"])
        require(artifacts.map { artifactKey(it) }.toSet() == reviewed.map { artifactKey(it as Map<*, *>) }.toSet()) {
            "Resolved release artifacts differ from reviewed license evidence. Refresh the runtime license review before packaging."
        }
        for ((name, expectedHash) in review["assetFiles"] as Map<*, *>) {
            val retained = file("src/main/assets/licenses/$name")
            val actualHash = MessageDigest.getInstance("SHA-256").digest(retained.readBytes())
                .joinToString("") { "%02x".format(it.toInt() and 0xff) }
            require(actualHash == expectedHash) { "Retained runtime license/NOTICE hash mismatch: $name" }
        }
        for ((name, evidence) in review["pomEvidence"] as Map<*, *>) {
            val retained = rootProject.file("licenses/poms/$name")
            val actualHash = MessageDigest.getInstance("SHA-256").digest(retained.readBytes())
                .joinToString("") { "%02x".format(it.toInt() and 0xff) }
            require(actualHash == (evidence as Map<*, *>)["sha256"]) { "Reviewed upstream POM hash mismatch: $name" }
        }
        val reviewHash = MessageDigest.getInstance("SHA-256").digest(reviewFile.readBytes())
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }
        output.writeText(groovy.json.JsonOutput.prettyPrint(groovy.json.JsonOutput.toJson(mapOf(
            "configuration" to "releaseRuntimeClasspath", "licenseReview" to "verified-reviewed-evidence",
            "reviewManifestSha256" to reviewHash,
            "artifacts" to artifacts))) + "\n")
    }
}

// Compile on the script's JDK 21, emit Java 17 bytecode; no extra JDK download.
kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.foundation)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.viewmodel.compose)
    debugImplementation(libs.androidx.compose.ui.tooling)
    testImplementation(libs.junit)
}
