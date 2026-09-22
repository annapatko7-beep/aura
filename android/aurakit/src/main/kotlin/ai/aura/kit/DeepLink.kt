package ai.aura.kit

import java.net.URI
import java.net.URLDecoder

/**
 * Разбор ссылок схемы aura:// (URL-схема приложения) — тот же набор маршрутов,
 * что на iOS (DeepLink.swift) и в docs/MOBILE_FEATURES.md:
 *
 *   aura://chats/{id}                        — открыть чат
 *   aura://voice                             — голосовой запрос
 *   aura://ask?text=...                      — сразу спросить Ауру
 *   aura://tasks                             — список задач
 *   aura://settings                          — настройки
 *   aura://oauth?provider=&code=&state=...   — возврат из Google OAuth
 *
 * Неизвестные пути → null (вызывающий код их игнорирует, без краша).
 */
sealed class DeepLink {
    data class Chats(val id: Long?) : DeepLink()
    data object Voice : DeepLink()
    data class Ask(val text: String) : DeepLink()
    data object Tasks : DeepLink()
    data object Notifications : DeepLink()
    data object Settings : DeepLink()
    data class OAuth(val provider: String, val code: String, val state: String) : DeepLink()

    companion object {
        const val SCHEME = "aura"

        fun parse(url: String): DeepLink? {
            val uri = try {
                URI(url)
            } catch (_: Exception) {
                return null
            }
            if (!SCHEME.equals(uri.scheme, ignoreCase = true)) return null
            val host = uri.host?.lowercase() ?: return null
            val query = parseQuery(uri.rawQuery)

            return when (host) {
                "chats" -> {
                    val id = uri.path?.trimStart('/')?.substringBefore('/')?.toLongOrNull()
                    Chats(id)
                }
                "voice" -> Voice
                "ask" -> Ask(query["text"] ?: "")
                "tasks" -> Tasks
                "notifications" -> Notifications
                "settings" -> Settings
                "oauth" -> {
                    val provider = query["provider"] ?: return null
                    val code = query["code"] ?: return null
                    val state = query["state"] ?: return null
                    OAuth(provider, code, state)
                }
                else -> null
            }
        }

        private fun parseQuery(rawQuery: String?): Map<String, String> {
            if (rawQuery.isNullOrEmpty()) return emptyMap()
            return rawQuery.split('&').mapNotNull { pair ->
                val parts = pair.split('=', limit = 2)
                val key = parts.getOrNull(0)?.takeIf { it.isNotEmpty() } ?: return@mapNotNull null
                val value = parts.getOrNull(1) ?: ""
                key to URLDecoder.decode(value, Charsets.UTF_8)
            }.toMap()
        }
    }
}
