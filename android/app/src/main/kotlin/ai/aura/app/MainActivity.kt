package ai.aura.app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import ai.aura.app.ui.LoginScreen
import ai.aura.app.ui.MainScaffold
import ai.aura.app.ui.OnboardingScreen
import ai.aura.app.ui.VoiceSheet
import ai.aura.app.ui.theme.AuraTheme

/**
 * Единственная activity приложения (singleTask — deep links и OAuth-редирект
 * aura://oauth приходят сюда же). Разрешения запрашиваем ровно в момент
 * действия: POST_NOTIFICATIONS — перед включением push, RECORD_AUDIO — при
 * нажатии кнопки микрофона.
 */
class MainActivity : ComponentActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        AppStore.bindContext(this)
        AppStore.init(this)
        Notifications.ensureChannel(this)

        val notificationPermission = registerForActivityResult(
            ActivityResultContracts.RequestPermission(),
        ) { granted -> if (granted) AppStore.registerPushDevice() }

        setContent {
            AuraTheme {
                val context = LocalContext.current
                val snackbar = remember { SnackbarHostState() }
                val errorText by AppStore.errorText.collectAsState()
                val busy by AppStore.busy.collectAsState()

                // Android 13+: системные уведомления (и push) требуют явного
                // разрешения — спрашиваем один раз при первом входе.
                LaunchedEffect(Unit) {
                    if (Build.VERSION.SDK_INT >= 33 &&
                        ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS)
                        != PackageManager.PERMISSION_GRANTED
                    ) {
                        notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
                    }
                }

                LaunchedEffect(errorText) {
                    errorText?.let {
                        snackbar.showSnackbar(it)
                        AppStore.clearError()
                    }
                }

                Scaffold(snackbarHost = { SnackbarHost(snackbar) }) { padding ->
                    Column(
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(padding),
                        verticalArrangement = Arrangement.Top,
                    ) {
                        if (busy) CircularProgressIndicator(modifier = Modifier.padding(8.dp))
                        val auth by AppStore.auth.collectAsState()
                        val needOnboarding by AppStore.needOnboarding.collectAsState()
                        when {
                            auth !is AppStore.AuthState.LoggedIn -> LoginScreen()
                            needOnboarding -> OnboardingScreen()
                            else -> MainScaffold()
                        }
                    }
                }
                VoiceSheet()
            }
        }

        intent?.dataString?.let { AppStore.handleDeepLink(it) }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        intent.dataString?.let { AppStore.handleDeepLink(it) }
    }
}
