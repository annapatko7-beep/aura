import java.io.FileInputStream
import java.util.Properties

// :app — Android-клиент Aura (Jetpack Compose). Секреты не храним: токены —
// в EncryptedSharedPreferences (Android Keystore), FCM-конфиг — в
// google-services.json, которого нет в репозитории (шаблон рядом).
plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.android)
    alias(libs.plugins.kotlin.compose)
}

// google-services применяется только если есть реальный конфиг Firebase —
// без него сборка не падает, а push честно показывает «не настроен».
val googleServicesFile = file("google-services.json")
if (googleServicesFile.exists()) {
    apply(plugin = "com.google.gms.google-services")
} else {
    logger.warn("app/google-services.json не найден — сборка без FCM (см. google-services.json.example)")
}

// Локальные свойства (не в git): адрес стенда для отладочной сборки.
val localProperties = Properties().apply {
    val file = rootProject.file("local.properties")
    if (file.exists()) load(FileInputStream(file))
}

android {
    namespace = "ai.aura.app"
    compileSdk = 34

    defaultConfig {
        applicationId = "ai.aura.app"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"
        // Адрес C++-сервера по умолчанию (без пути, как у desktop/iOS-клиентов);
        // переопределяется в local.properties (aura.server.url=ws://10.0.2.2:9000
        // для эмулятора) и в настройках приложения. Слушатель: docs/SETUP.md.
        buildConfigField(
            "String",
            "DEFAULT_SERVER_URL",
            "\"${localProperties.getProperty("aura.server.url", "ws://10.0.2.2:9000")}\"",
        )
    }

    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
        }
        debug {
            isMinifyEnabled = false
        }
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
    packaging {
        resources {
            excludes += "/META-INF/{AL2.0,LGPL2.1}"
        }
    }
}

dependencies {
    implementation(project(":aurakit"))
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.material.icons)
    implementation(libs.androidx.lifecycle.runtime)
    implementation(libs.androidx.security.crypto)
    implementation(libs.androidx.browser)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.kotlinx.coroutines.android)
    // FCM: библиотека на месте всегда; без google-services.json FirebaseApp
    // не инициализируется и клиент это обрабатывает (без крашей).
    implementation(platform(libs.firebase.bom))
    implementation(libs.firebase.messaging)
    debugImplementation(libs.androidx.compose.ui.tooling)
}
