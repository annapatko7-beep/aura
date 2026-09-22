package ai.aura.app.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.FilterChip
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import ai.aura.app.AppStore
import ai.aura.app.ui.theme.GlassPanel

/**
 * Онбординг-опрос после регистрации: день рождения, аллергии, диета, город,
 * транспорт, бюджет. Всё — необязательные поля (кнопка «Пропустить»), но
 * заполненная анкета сразу попадает в контекст Ауры (prefs → AI context).
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

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(24.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Text("Расскажите о себе", style = MaterialTheme.typography.headlineSmall)
        Text(
            "Аура использует это в плане встреч и рекомендаций. Всё можно " +
                "изменить позже в настройках, всё необязательно.",
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        GlassPanel(modifier = Modifier.fillMaxWidth()) {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                OutlinedTextField(
                    value = birthday, onValueChange = { birthday = it },
                    label = { Text("День рождения (ГГГГ-ММ-ДД)") },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedTextField(
                    value = allergies, onValueChange = { allergies = it },
                    label = { Text("Аллергии (через запятую)") },
                    modifier = Modifier.fillMaxWidth(),
                )
                Text("Диета", style = MaterialTheme.typography.titleSmall)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    dietOptions.forEach { option ->
                        FilterChip(
                            selected = diet.contains(option),
                            onClick = {
                                diet = if (diet.contains(option)) diet - option else diet + option
                            },
                            label = { Text(dietLabel(option)) },
                        )
                    }
                }
                Text("Передвижение", style = MaterialTheme.typography.titleSmall)
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    transportOptions.forEach { (value, label) ->
                        FilterChip(
                            selected = transport == value,
                            onClick = { transport = value },
                            label = { Text(label) },
                        )
                    }
                }
                OutlinedTextField(
                    value = city, onValueChange = { city = it },
                    label = { Text("Город") },
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedTextField(
                    value = budget, onValueChange = { budget = it },
                    label = { Text("Бюджет на встречу, € (например 25)") },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
                    modifier = Modifier.fillMaxWidth(),
                )
                Button(
                    onClick = {
                        AppStore.submitOnboarding(
                            birthday = birthday.trim(),
                            allergies = allergies.split(',')
                                .map { it.trim() }.filter { it.isNotEmpty() },
                            diet = diet.toList(),
                            transport = transport,
                            city = city.trim(),
                            budget = budget.trim(),
                        )
                    },
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("Сохранить") }
                TextButton(
                    onClick = { AppStore.skipOnboarding() },
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("Пропустить") }
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
