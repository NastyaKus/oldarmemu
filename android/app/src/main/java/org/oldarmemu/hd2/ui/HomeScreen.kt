@file:OptIn(ExperimentalMaterial3Api::class, ExperimentalLayoutApi::class)

package org.oldarmemu.hd2.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Check
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.foundation.Image
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material3.IconButton
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp

@Composable
fun HomeScreen(state: AppState, importRecovery: () -> Unit, addToSd: () -> Unit) {
    @Suppress("UNUSED_VARIABLE") val v = state.version   // re-read on changes
    val st = state.st
    val layout = st.nandLayout()
    var layoutDialog by remember { mutableStateOf(false) }
    if (layoutDialog) NandLayoutDialog(state) { layoutDialog = false }

    Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = 24.dp)) {
        ScreenTitle("HTC HD2", "Qualcomm Snapdragon QSD8250 · 1 ГГц Scorpion · Adreno 200")

        /* hero card: the phone and its state */
        ElevatedCard(
            shape = RoundedCornerShape(28.dp),
            modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
        ) {
            Box(
                Modifier.background(Brush.linearGradient(listOf(
                    MaterialTheme.colorScheme.primaryContainer,
                    MaterialTheme.colorScheme.tertiaryContainer,
                ))).padding(20.dp),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Hd2Art(state.lastFrame(), width = 112.dp)
                    Spacer(Modifier.width(20.dp))
                    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                        Text(if (st.nandUsed) "Прошивка установлена" else "NAND пуста",
                            style = MaterialTheme.typography.titleLarge,
                            color = MaterialTheme.colorScheme.onPrimaryContainer)
                        StatusLine(Hd2Icons.Memory,
                            "NAND ${layout.sizeMb} МБ · system ${layout.systemMb}")
                        StatusLine(Icons.Filled.Build,
                            if (st.recovery.exists()) "Recovery импортирован" else "Recovery нет")
                        StatusLine(Hd2Icons.Bolt,
                            if (st.gpu) "Графика: GPU Adreno" else "Графика: программная")
                        StatusLine(Hd2Icons.SdCard,
                            "SD: ${st.sdcardEntries().size} файл(ов)")
                    }
                }
            }
        }

        /* the two ways to start the phone */
        Row(
            Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 8.dp),
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Button(
                onClick = { state.launch("android") },
                modifier = Modifier.weight(1f).height(64.dp),
                shape = RoundedCornerShape(20.dp),
                contentPadding = ButtonDefaults.ButtonWithIconContentPadding,
            ) {
                Icon(Icons.Filled.PlayArrow, null)
                Spacer(Modifier.width(8.dp))
                Text("Android", style = MaterialTheme.typography.titleMedium)
            }
            FilledTonalButton(
                onClick = { state.launch("recovery") },
                modifier = Modifier.weight(1f).height(64.dp),
                shape = RoundedCornerShape(20.dp),
            ) {
                Icon(Icons.Filled.Build, null)
                Spacer(Modifier.width(8.dp))
                Text("Recovery", style = MaterialTheme.typography.titleMedium)
            }
        }

        /* saved states: continue where the phone was left */
        val saved = state.snaps.slots().filter { it.exists }
        if (saved.isNotEmpty()) {
            SectionHeader("Снимки состояния")
            Card(
                shape = RoundedCornerShape(24.dp),
                colors = CardDefaults.cardColors(MaterialTheme.colorScheme.surfaceContainer),
                modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp),
            ) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    for (slot in saved) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            val pic = remember(slot.time) { slot.preview() }
                            Box(Modifier.size(36.dp, 60.dp).clip(RoundedCornerShape(6.dp))
                                .background(androidx.compose.ui.graphics.Color.Black)) {
                                pic?.let { Image(it.asImageBitmap(), null) }
                            }
                            Spacer(Modifier.width(12.dp))
                            Column(Modifier.weight(1f)) {
                                Text("Слот ${slot.index}", style = MaterialTheme.typography.titleSmall)
                                Text(SimpleDateFormat("d MMM, HH:mm", Locale("ru")).format(Date(slot.time)) +
                                    " · " + formatSize(slot.bytes),
                                    style = MaterialTheme.typography.bodySmall)
                            }
                            IconButton({ state.snaps.delete(slot.index); state.refresh() }) {
                                Icon(Icons.Filled.Delete, "Удалить")
                            }
                            FilledTonalButton({ state.launchSnapshot(slot.index) }) {
                                Text("Продолжить")
                            }
                        }
                    }
                }
            }
        }

        /* installation guide */
        SectionHeader("Установка прошивки")
        Card(
            shape = RoundedCornerShape(24.dp),
            colors = CardDefaults.cardColors(MaterialTheme.colorScheme.surfaceContainer),
            modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp),
        ) {
            Column(Modifier.padding(vertical = 8.dp)) {
                Step(1, st.recovery.exists(), "Recovery",
                    "CWM или TWRP для NAND (.img или .zip)", "Импорт", importRecovery)
                Step(2, true, "Разметка NAND",
                    "${layout.label}. Для ICS/JB/KK нужно 1024 МБ", "Изменить") { layoutDialog = true }
                Step(3, state.romOnSd, "Прошивка на SD-карте",
                    "zip прошивки; zz-hd2emu-compat.zip появится сам", "Добавить", addToSd)
                Step(4, st.nandUsed, "Прошить в Recovery",
                    "install zip → прошивка, затем zz-hd2emu-compat.zip", "Recovery") {
                    state.launch("recovery")
                }
                Step(5, false, "Запустить Android", "Первая загрузка идёт несколько минут",
                    "Запуск", last = true) { state.launch("android") }
            }
        }

        /* hints */
        SectionHeader("Подсказки")
        FlowRow(
            Modifier.padding(horizontal = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Hint(Icons.Filled.Info, "В recovery: громкость — навигация, «Вызов» — выбор")
            Hint(Hd2Icons.Power, "«Питание» будит экран, долгое нажатие — меню")
            Hint(Hd2Icons.Phone, "В AROMA выберите system@NAND, data@NAND")
        }
    }
}

@Composable
private fun StatusLine(icon: ImageVector, text: String) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Icon(icon, null, Modifier.size(18.dp), tint = MaterialTheme.colorScheme.onPrimaryContainer)
        Spacer(Modifier.width(8.dp))
        Text(text, style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onPrimaryContainer)
    }
}

@Composable
private fun Step(n: Int, done: Boolean, title: String, summary: String, action: String,
                 onClick: () -> Unit) = Step(n, done, title, summary, action, false, onClick)

@Composable
private fun Step(n: Int, done: Boolean, title: String, summary: String, action: String,
                 last: Boolean, onClick: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier.size(36.dp).clip(CircleShape).background(
                if (done) MaterialTheme.colorScheme.primary
                else MaterialTheme.colorScheme.surfaceContainerHigh),
            contentAlignment = Alignment.Center,
        ) {
            if (done) Icon(Icons.Filled.Check, null, tint = MaterialTheme.colorScheme.onPrimary)
            else Text("$n", fontWeight = FontWeight.Bold)
        }
        Column(Modifier.weight(1f).padding(horizontal = 14.dp)) {
            Text(title, style = MaterialTheme.typography.titleMedium)
            Text(summary, style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        TextButton(onClick) { Text(action) }
    }
    @Suppress("UNUSED_EXPRESSION") last
}

@Composable
private fun Hint(icon: ImageVector, text: String) {
    Card(
        shape = RoundedCornerShape(16.dp),
        colors = CardDefaults.cardColors(MaterialTheme.colorScheme.secondaryContainer),
    ) {
        Row(Modifier.padding(horizontal = 12.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically) {
            Icon(icon, null, Modifier.size(18.dp))
            Spacer(Modifier.width(8.dp))
            Text(text, style = MaterialTheme.typography.bodySmall)
        }
    }
}
