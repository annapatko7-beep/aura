# Aura Android — правила R8.
# kotlinx.serialization и OkHttp поставляют собственные consumer-rules;
# здесь только страховка для отражения в сериализуемых кадрах протокола.
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.**
-keepclassmembers class ai.aura.kit.** {
    *** Companion;
}
-keepclasseswithmembers class ai.aura.kit.** {
    kotlinx.serialization.KSerializer serializer(...);
}
