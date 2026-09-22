// :aurakit — общий слой протокола Aura для Android (аналог AuraKit на iOS).
// Чистый Kotlin/JVM без Android-зависимостей: транспорт WS, типизированный
// клиент и разбор deep links. Тесты — обычные JVM-тесты (`:aurakit:test`).
plugins {
    alias(libs.plugins.kotlin.jvm)
    alias(libs.plugins.kotlin.serialization)
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(17))
    }
}

dependencies {
    implementation(libs.okhttp)
    implementation(libs.kotlinx.serialization.json)
    implementation(libs.kotlinx.coroutines.core)
    testImplementation(libs.junit)
}
