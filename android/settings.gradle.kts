// Selfvoy для Android — корневой проект (этап 11).
// Два модуля: :selfvoykit (чистый Kotlin/JVM — протокол, без Android-зависимостей,
// поэтому его тесты запускаются обычным `./gradlew :selfvoykit:test`) и :app
// (Jetpack Compose UI). Тот же принцип, что на iOS: общий только протокол.
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

rootProject.name = "selfvoy-android"
include(":app")
include(":selfvoykit")
