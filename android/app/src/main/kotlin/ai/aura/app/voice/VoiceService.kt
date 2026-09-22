package ai.aura.app.voice

import android.content.Context
import android.content.Intent
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Bundle
import android.speech.RecognitionListener
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer
import android.speech.tts.TextToSpeech
import android.util.Base64
import ai.aura.app.AppStore
import ai.aura.kit.AuraError
import ai.aura.kit.str
import java.io.ByteArrayOutputStream
import java.util.Locale

/**
 * Голосовой ввод и озвучка — гибрид, как на iOS (docs/PROTOCOL.md «Речь»):
 *  1. on-device: системный SpeechRecognizer (без разрешений сверх RECORD_AUDIO);
 *  2. fallback: запись 16 кГц моно 16 бит → WAV → base64 → speech.transcribe
 *     (серверный Whisper-совместимый STT).
 * Озвучка ответов — платформенный TextToSpeech (ru, иначе en).
 */
class VoiceService(private val context: Context) {

    /** Результат распознавания. */
    sealed class Result {
        data class Text(val text: String, val onDevice: Boolean) : Result()
        data class Failure(val message: String) : Result()
    }

    private var recognizer: SpeechRecognizer? = null

    val onDeviceAvailable: Boolean
        get() = SpeechRecognizer.isRecognitionAvailable(context)

    /** Распознавание: сначала on-device, при отсутствии/ошибке — серверный STT. */
    suspend fun transcribe(language: String = "ru-RU"): Result {
        if (onDeviceAvailable) {
            val onDevice = recognizeOnDevice(language)
            if (onDevice != null) return Result.Text(onDevice, onDevice = true)
        }
        return try {
            val wav = recordWav(maxSeconds = 15)
            if (wav.isEmpty()) return Result.Failure("не удалось записать звук")
            val payload = AppStore.client.speechTranscribe(
                audioBase64 = Base64.encodeToString(wav, Base64.NO_WRAP),
                language = "auto",
                format = "wav",
            )
            val text = payload.str("text")
            if (text.isBlank()) Result.Failure("пустая расшифровка") else Result.Text(text, onDevice = false)
        } catch (error: AuraError) {
            Result.Failure(error.message)
        } catch (error: Exception) {
            Result.Failure(error.message ?: "микрофон недоступен")
        }
    }

    fun release() {
        recognizer?.destroy()
        recognizer = null
    }

    // ------------------------------------------------------------- on-device

    private suspend fun recognizeOnDevice(language: String): String? =
        kotlinx.coroutines.suspendCancellableCoroutine { continuation ->
            val speech = SpeechRecognizer.createSpeechRecognizer(context)
            recognizer = speech
            val intent = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH).apply {
                putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
                putExtra(RecognizerIntent.EXTRA_LANGUAGE, language)
                putExtra(RecognizerIntent.EXTRA_MAX_RESULTS, 1)
            }
            speech.setRecognitionListener(object : RecognitionListener {
                override fun onReadyForSpeech(params: Bundle?) = Unit
                override fun onBeginningOfSpeech() = Unit
                override fun onRmsChanged(rmsdB: Float) = Unit
                override fun onBufferReceived(buffer: ByteArray?) = Unit
                override fun onEndOfSpeech() = Unit
                override fun onPartialResults(partialResults: Bundle?) = Unit
                override fun onEvent(eventType: Int, params: Bundle?) = Unit

                override fun onError(error: Int) {
                    if (continuation.isActive) continuation.resumeWith(Result.success(null))
                }

                override fun onResults(results: Bundle?) {
                    val text = results?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)
                        ?.firstOrNull()
                    if (continuation.isActive) continuation.resumeWith(Result.success(text))
                }
            })
            continuation.invokeOnCancellation { speech.destroy() }
            speech.startListening(intent)
        }

    // ------------------------------------------------------------- серверный STT

    /** Запись микрофона в WAV (16 кГц, моно, 16 бит) — формат AI-сервиса. */
    private fun recordWav(maxSeconds: Int): ByteArray {
        val sampleRate = 16_000
        val minBuffer = AudioRecord.getMinBufferSize(
            sampleRate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
        )
        if (minBuffer <= 0) return ByteArray(0)
        val record = try {
            AudioRecord(
                MediaRecorder.AudioSource.VOICE_RECOGNITION,
                sampleRate, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT,
                minBuffer * 2,
            )
        } catch (_: SecurityException) {
            return ByteArray(0)  // RECORD_AUDIO не выдан
        }
        val pcm = ByteArrayOutputStream()
        val chunk = ByteArray(minBuffer)
        try {
            record.startRecording()
            val deadline = System.currentTimeMillis() + maxSeconds * 1000L
            while (System.currentTimeMillis() < deadline) {
                val read = record.read(chunk, 0, chunk.size)
                if (read > 0) pcm.write(chunk, 0, read)
            }
        } finally {
            record.stop()
            record.release()
        }
        return wavWrap(pcm.toByteArray(), sampleRate)
    }

    private fun wavWrap(pcm: ByteArray, sampleRate: Int): ByteArray {
        val out = ByteArrayOutputStream()
        val byteRate = sampleRate * 2
        fun intLE(value: Int) = byteArrayOf(
            (value and 0xFF).toByte(), ((value shr 8) and 0xFF).toByte(),
            ((value shr 16) and 0xFF).toByte(), ((value shr 24) and 0xFF).toByte(),
        )
        fun shortLE(value: Int) = byteArrayOf((value and 0xFF).toByte(), ((value shr 8) and 0xFF).toByte())
        out.write("RIFF".toByteArray())
        out.write(intLE(36 + pcm.size))
        out.write("WAVEfmt ".toByteArray())
        out.write(intLE(16)); out.write(shortLE(1)); out.write(shortLE(1))
        out.write(intLE(sampleRate)); out.write(intLE(byteRate))
        out.write(shortLE(2)); out.write(shortLE(16))
        out.write("data".toByteArray()); out.write(intLE(pcm.size))
        out.write(pcm)
        return out.toByteArray()
    }
}

/** Озвучка ответа Ауры (платформенный TTS, как AVSpeechSynthesizer на iOS). */
fun speak(context: Context, text: String) {
    if (text.isBlank()) return
    lateinit var tts: TextToSpeech
    tts = TextToSpeech(context) { status ->
        if (status == TextToSpeech.SUCCESS) {
            val locale = listOf(Locale("ru"), Locale.ENGLISH).firstOrNull {
                tts.isLanguageAvailable(it) >= TextToSpeech.LANG_AVAILABLE
            }
            locale?.let { tts.language = it }
            tts.speak(text, TextToSpeech.QUEUE_FLUSH, null, "aura-reply")
        }
        tts.shutdown()
    }
}
