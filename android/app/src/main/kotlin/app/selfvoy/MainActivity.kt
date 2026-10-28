package app.selfvoy

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
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
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
import androidx.core.content.ContextCompat
import app.selfvoy.ui.LoginScreen
import app.selfvoy.ui.MainScaffold
import app.selfvoy.ui.OnboardingScreen
import app.selfvoy.ui.VoiceSheet
import app.selfvoy.ui.theme.SelfvoyTheme
import app.selfvoy.ui.theme.BgDeep

/**
 * Единственная activity приложения (singleTask — deep links и OAuth-редирект
 * selfvoy://oauth приходят сюда же). Разрешения запрашиваем ровно в момент
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
            SelfvoyTheme {
                val context = LocalContext.current
                val snackbar = remember { SnackbarHostState() }
                val errorText by AppStore.errorText.collectAsState()

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

                // Фон и отступы под системные панели рисуют сами экраны (SelfvoyBackground);
                // состояние «занято» показывают кнопки, а не общий спиннер.
                Scaffold(
                    snackbarHost = { SnackbarHost(snackbar) },
                    containerColor = BgDeep,
                    contentWindowInsets = WindowInsets(0, 0, 0, 0),
                ) { padding ->
                    Column(
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(padding),
                        verticalArrangement = Arrangement.Top,
                    ) {
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
