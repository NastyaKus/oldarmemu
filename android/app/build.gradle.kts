plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
    id("app.cash.paparazzi")
}

android {
    namespace = "org.oldarmemu.hd2"
    compileSdk = 35
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "org.oldarmemu.hd2"
        minSdk = 28
        targetSdk = 34          // QEMU runs from nativeLibraryDir (exec allowed there)
        versionCode = (System.getenv("VERSION_CODE") ?: "16").toInt()
        versionName = System.getenv("VERSION_NAME") ?: "0.5.3"
        ndk { abiFilters += listOf("arm64-v8a") }
    }

    signingConfigs {
        create("dev") {
            storeFile = file(System.getenv("KEYSTORE") ?: "dev.keystore")
            storePassword = "oldarmemu"
            keyAlias = "dev"
            keyPassword = "oldarmemu"
        }
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
            signingConfig = signingConfigs.getByName("dev")
        }
        getByName("debug") {
            signingConfig = signingConfigs.getByName("dev")
        }
    }

    packaging {
        // QEMU is an executable shipped as lib*.so; it must be extracted
        jniLibs.useLegacyPackaging = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
}

dependencies {
    val bom = platform("androidx.compose:compose-bom:2024.12.01")
    implementation(bom)
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.material:material-icons-core")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.navigation:navigation-compose:2.8.5")
    implementation("androidx.core:core-ktx:1.13.1")
    testImplementation("junit:junit:4.13.2")
}

tasks.withType<Test> {
    systemProperty("fat.src", System.getProperty("fat.src") ?: "")
    systemProperty("fat.out", System.getProperty("fat.out") ?: "")
    systemProperty("launcher.in", System.getProperty("launcher.in") ?: "")
    systemProperty("launcher.out", System.getProperty("launcher.out") ?: "")
}
