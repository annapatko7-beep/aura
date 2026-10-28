package app.selfvoy.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import app.selfvoy.AppStore
import app.selfvoy.ui.theme.SelfvoyBackground
import app.selfvoy.ui.theme.SelfvoyChip
import app.selfvoy.ui.theme.SelfvoyEmblem
import app.selfvoy.ui.theme.SelfvoyField
import app.selfvoy.ui.theme.GlassButton
import app.selfvoy.ui.theme.GlassCard
import app.selfvoy.ui.theme.GradientButton
import app.selfvoy.ui.theme.Muted
import app.selfvoy.ui.theme.TextPrimary
import app.selfvoy.ui.theme.TextSecondary

/**
 * Онбординг-опрос после регистрации: день рождения, аллергии, диета, город,
 * транспорт, бюджет. Всё — необязательные поля (кнопка «Пропустить»), но
 * заполненная анкета сразу попадает в контекст Selfvoy (prefs → AI context).
 * Сервер по факту заполнения ставит флаг onboarded (docs/PROTOCOL.md).
 */
@OptIn(ExperimentalLayoutApi::class)
@Composable
fun OnboardingScreen() {
    var birthday by remember { mutableStateOf("") }
    var allergies by remember { mutableStateOf("") }
    var city by remember { mutableStateOf("") }
    var budget by remember { mutableStateOf("") }
    val dietOptions = listOf("vegan", "vegetarian", "gluten_free", "lactose_free", "halal", "kosher")
    var diet by remember { mutableStateOf(setOf<String>()) }
    val transportOptions = listOf("walk" to "Пешком", "bike" to "Велосипед",
        "transit" to "Транспорт", "car" to "Машина")
    var transport by remember { mutableStateOf("walk") }

    SelfvoyBackground {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .statusBarsPadding()
                .imePadding()
                .verticalScroll(rememberScrollState())
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            GlassCard(Modifier.fillMaxWidth(), radius = 34.dp, contentPadding = PaddingValues(24.dp)) {
                Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
                    SelfvoyEmblem(56.dp, Modifier.align(Alignment.CenterHorizontally))
                    Text("Расскажите о себе", style = MaterialTheme.typography.headlineMedium, color = TextPrimary,
                        textAlign = TextAlign.Center, modifier = Modifier.fillMaxWidth())
                    Muted("Selfvoy использует это для мест, времени и рекомендаций. Всё необязательно и меняется в настройках.",
                        Modifier.fillMaxWidth(), align = TextAlign.Center)
                    SelfvoyField(birthday, { birthday = it }, "ГГГГ-ММ-ДД", label = "День рождения",
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number))
                    SelfvoyField(allergies, { allergies = it }, "через запятую", label = "Аллергии")
                    Text("Диета", style = MaterialTheme.typography.labelMedium, color = TextSecondary)
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        dietOptions.forEach { option ->
                            SelfvoyChip(dietLabel(option), active = diet.contains(option)) {
                                diet = if (diet.contains(option)) diet - option else diet + option
                            }
                        }
                    }
                    Text("Передвижение", style = MaterialTheme.typography.labelMedium, color = TextSecondary)
                    FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                        transportOptions.forEach { (value, label) ->
                            SelfvoyChip(label, active = transport == value) { transport = value }
                        }
                    }
                    SelfvoyField(city, { city = it }, "Например, Москва", label = "Город")
                    SelfvoyField(budget, { budget = it }, "Например, 25", label = "Бюджет на встречу",
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal))
                    GradientButton(
                        "Сохранить",
                        {
                            AppStore.submitOnboarding(
                                birthday = birthday.trim(),
                                allergies = allergies.split(',').map { it.trim() }.filter { it.isNotEmpty() },
                                diet = diet.toList(),
                                transport = transport,
                                city = city.trim(),
                                budget = budget.trim(),
                            )
                        },
                        Modifier.fillMaxWidth(),
                    )
                    GlassButton("Пропустить", { AppStore.skipOnboarding() }, Modifier.fillMaxWidth(), quiet = true)
                }
            }
        }
    }
}

private fun dietLabel(option: String): String = when (option) {
    "vegan" -> "Веган"
    "vegetarian" -> "Вегетарианец"
    "gluten_free" -> "Без глютена"
    "lactose_free" -> "Без лактозы"
    "halal" -> "Халяль"
    "kosher" -> "Кошер"
    else -> option
}
