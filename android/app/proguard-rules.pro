# Selfvoy Android — правила R8.
# kotlinx.serialization и OkHttp поставляют собственные consumer-rules;
# здесь только страховка для отражения в сериализуемых кадрах протокола.
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.**
-keepclassmembers class app.selfvoy.kit.** {
    *** Companion;
}
-keepclasseswithmembers class app.selfvoy.kit.** {
    kotlinx.serialization.KSerializer serializer(...);
}
