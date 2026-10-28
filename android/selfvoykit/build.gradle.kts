// :selfvoykit — общий слой протокола Selfvoy для Android (аналог SelfvoyKit на iOS).
// Чистый Kotlin/JVM без Android-зависимостей: транспорт WS, типизированный
// клиент и разбор deep links. Тесты — обычные JVM-тесты (`:selfvoykit:test`).
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
