package ai.aura.app.ui

import android.Manifest
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import ai.aura.app.AppStore
import ai.aura.app.voice.VoiceService
import ai.aura.app.voice.speak
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Голосовой оверлей: слушает (on-device SpeechRecognizer или серверный STT),
 * показывает расшифровку для правки и отправляет Ауре. Ответ озвучивается.
 * RECORD_AUDIO запрашивается здесь же, в момент действия.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun VoiceSheet() {
    val wantVoice by AppStore.wantVoice.collectAsState()
    if (!wantVoice) return

    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val service = remember { VoiceService(context) }
    var state by remember { mutableStateOf("Нажмите «Слушать»") }
    var text by remember { mutableStateOf("") }

    val permission = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { /* результат обработает повторное нажатие «Слушать» */ }

    ModalBottomSheet(
        onDismissRequest = {
            service.release()
            AppStore.consumeWantVoice()
        },
        sheetState = sheetState,
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(24.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text("Голосовой запрос", style = MaterialTheme.typography.titleMedium)
            Text(state, color = MaterialTheme.colorScheme.onSurfaceVariant)
            if (text.isNotEmpty()) {
                OutlinedTextField(
                    value = text, onValueChange = { text = it },
                    label = { Text("Проверьте расшифровку") },
                    modifier = Modifier.fillMaxWidth(),
                )
                Button(
                    onClick = {
                        val question = text
                        AppStore.ask(question)
                        // Озвучим ответ, как только он придёт (first — и выходим).
                        scope.launch {
                            val reply = AppStore.askReply.first { it.isNotEmpty() }
                            speak(context, reply)
                        }
                        service.release()
                        AppStore.consumeWantVoice()
                    },
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("Спросить Ауру") }
            } else {
                Button(
                    onClick = {
                        if (ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO)
                            != PackageManager.PERMISSION_GRANTED
                        ) {
                            permission.launch(Manifest.permission.RECORD_AUDIO)
                            state = "Нужно разрешение на микрофон"
                        } else {
                            state = "Слушаю…"
                            scope.launch {
                                val result = withContext(Dispatchers.IO) { service.transcribe() }
                                when (result) {
                                    is VoiceService.Result.Text -> {
                                        text = result.text
                                        state = if (result.onDevice) "Распознано на устройстве"
                                        else "Распознано на сервере"
                                    }
                                    is VoiceService.Result.Failure -> state = result.message
                                }
                            }
                        }
                    },
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("Слушать") }
            }
            OutlinedButton(
                onClick = {
                    service.release()
                    AppStore.consumeWantVoice()
                },
                modifier = Modifier.fillMaxWidth(),
            ) { Text("Закрыть") }
        }
    }
}
