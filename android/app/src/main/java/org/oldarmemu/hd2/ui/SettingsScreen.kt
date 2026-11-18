package org.oldarmemu.hd2.ui

import android.os.Build
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.List
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material.icons.filled.Share
import androidx.compose.material.icons.filled.Star
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import org.oldarmemu.hd2.BuildConfig
import org.oldarmemu.hd2.Settings
import kotlin.concurrent.thread

@Composable
fun SettingsScreen(
    state: AppState,
    backupNand: () -> Unit,
    restoreNand: () -> Unit,
    shareLogs: () -> Unit,
    openLog: (String) -> Unit,
) {
    @Suppress("UNUSED_VARIABLE") val v = state.version
    val cfg = state.cfg
    val st = state.st
    var layoutDialog by remember { mutableStateOf(false) }
    var wipe by remember { mutableStateOf(false) }
    if (layoutDialog) NandLayoutDialog(state) { layoutDialog = false }
    if (wipe) ConfirmDialog("Стереть всю память NAND? Установленная прошивка пропадёт.",
        "Стереть", { wipe = false }) { st.wipeNand(); state.refresh() }

    Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = 32.dp)) {
        ScreenTitle("Настройки")

        SectionHeader("Экран и управление")
        SettingsGroup {
            ChoiceRow(Hd2Icons.Phone, "Масштаб экрана",
                listOf(Settings.SCALE_FIT, Settings.SCALE_FILL, Settings.SCALE_INTEGER),
                { when (it) { Settings.SCALE_FIT -> "Вписать"; Settings.SCALE_FILL -> "Растянуть"; else -> "Целый" } },
                cfg.scaleMode, "«Целый» — пиксель в пиксель, без размытия") { cfg.scaleMode = it }
            SwitchRow(null, "Сглаживание картинки", "Мягче при масштабировании",
                cfg.smooth) { cfg.smooth = it }
            SwitchRow(null, "Счётчик кадров", "к/с в углу экрана", cfg.showFps) { cfg.showFps = it }
            Divider()
            SwitchRow(Hd2Icons.Power, "Боковые кнопки", "Громкость и питание над экраном",
                cfg.sideButtons) { cfg.sideButtons = it }
            SwitchRow(Hd2Icons.Windows, "Кнопки под экраном", "Вызов, Домой, Windows, Назад, Сброс",
                cfg.bottomButtons) { cfg.bottomButtons = it }
            SliderRow(null, "Размер кнопок", 36..72, 4, cfg.buttonSize, "dp") { cfg.buttonSize = it }
            SwitchRow(null, "Вибрация кнопок", null, cfg.haptics) { cfg.haptics = it }
            SwitchRow(Hd2Icons.VolumeUp, "Кнопки громкости телефона",
                "Работают как кнопки громкости HD2", cfg.volumeKeys) { cfg.volumeKeys = it }
            Divider()
            SwitchRow(Icons.Filled.Menu, "Полноэкранный режим", "Скрыть панели системы",
                cfg.immersive) { cfg.immersive = it }
            SwitchRow(Icons.Filled.Lock, "Не гасить экран", "Пока работает эмулятор",
                cfg.keepScreenOn) { cfg.keepScreenOn = it }
        }

        SectionHeader("Производительность")
        SettingsGroup {
            ChoiceRow(Hd2Icons.Bolt, "Графика", listOf(true, false),
                { if (it) "GPU Adreno" else "Программная" }, st.gpu,
                "Применяется после прошивки zz-hd2emu-compat.zip") { gpu ->
                thread { st.gpu = gpu }
                state.message = if (st.hasCompatZip())
                    "zz-hd2emu-compat.zip пересобран. Прошейте его в recovery, чтобы применить."
                else "Добавьте прошивку на SD-карту: zz-hd2emu-compat.zip появится с этой настройкой."
            }
            ChoiceRow(Hd2Icons.Bolt, "Отрисовка Adreno",
                listOf(Settings.RENDER_GL, Settings.RENDER_SW),
                { if (it == Settings.RENDER_GL) "GPU телефона" else "Процессор" }, cfg.renderer,
                "GPU телефона — быстрее; при сбое эмулятор сам перейдёт на процессор. " +
                    "Применяется при следующем запуске") { cfg.renderer = it }
            ChoiceRow(Hd2Icons.Memory, "Кэш трансляции кода", Settings.TB_SIZES,
                { "$it МБ" }, cfg.tbSize, "Больше — меньше перекомпиляций, но больше памяти") {
                cfg.tbSize = it
            }
            SwitchRow(Hd2Icons.Bolt, "Большие ядра процессора",
                "Держать эмулятор на самых быстрых ядрах", cfg.pinCpu) { cfg.pinCpu = it }
            SwitchRow(null, "Процессор HD2 на самом быстром ядре",
                "Отдать ему одному ядро X3/Prime, остальное — на соседние. " +
                    "Сравните к/с: при нагреве ядро может сбавлять частоту",
                cfg.vcpuPrime) { cfg.vcpuPrime = it }
        }

        SectionHeader("Сеть")
        SettingsGroup {
            SwitchRow(Hd2Icons.Wifi, "Wi‑Fi через интернет телефона",
                "В HD2 включите Wi‑Fi и подключитесь к сети «HD2-Emulator»",
                cfg.wifi) { cfg.wifi = it }
        }

        SectionHeader("Память и файлы")
        SettingsGroup {
            ClickRow(Hd2Icons.Memory, "Разметка NAND", st.nandLayout().label) { layoutDialog = true }
            ChoiceRow(Hd2Icons.SdCard, "Размер SD-карты", Settings.SD_SIZES,
                { if (it >= 1024) "${it / 1024} ГБ" else "$it МБ" }, cfg.sdSizeMb,
                "Образ пересоздаётся при запуске; файлы сохраняются") {
                cfg.sdSizeMb = it
            }
            Divider()
            ClickRow(Hd2Icons.Upload, "Бэкап NAND", "Сохранить прошивку и данные в файл",
                backupNand)
            ClickRow(Hd2Icons.Download, "Восстановить NAND", "Из файла бэкапа", restoreNand)
            ClickRow(Icons.Filled.Delete, "Стереть NAND", "Как новый телефон") { wipe = true }
        }

        SectionHeader("Звук и журналы")
        SettingsGroup {
            SwitchRow(Hd2Icons.VolumeUp, "Звук", null, cfg.sound) { cfg.sound = it }
            ChoiceRow(null, "Буфер звука", Settings.LATENCIES, { "$it мс" }, cfg.audioLatencyMs,
                "Меньше — меньше задержка, больше — без треска") { cfg.audioLatencyMs = it }
            Divider()
            ClickRow(Icons.Filled.List, "Журнал ядра", "ram_console HD2") { openLog("console") }
            ClickRow(Hd2Icons.Doc, "Журнал эмулятора", "qemu.log") { openLog("qemu") }
            ClickRow(Icons.Filled.Share, "Поделиться журналами", "Для отчёта об ошибке", shareLogs)
        }

        SectionHeader("Оформление")
        SettingsGroup {
            ChoiceRow(Hd2Icons.Palette, "Тема",
                listOf(Settings.THEME_SYSTEM, Settings.THEME_LIGHT, Settings.THEME_DARK),
                { when (it) { Settings.THEME_LIGHT -> "Светлая"; Settings.THEME_DARK -> "Тёмная"; else -> "Системная" } },
                cfg.theme) { cfg.theme = it; state.themeVersion++ }
            if (Build.VERSION.SDK_INT >= 31) {
                SwitchRow(Icons.Filled.Star, "Цвета обоев", "Material You",
                    cfg.dynamicColor) { cfg.dynamicColor = it; state.themeVersion++ }
            }
        }

        SectionHeader("О программе")
        SettingsGroup {
            ClickRow(Icons.Filled.Info, "HD2 Emulator ${BuildConfig.VERSION_NAME}",
                "QEMU 10.1 · машина htc-leo · эмуляция Adreno 200") {}
            ClickRow(Icons.Filled.Build, "Как это работает",
                "Настоящие прошивки HD2 запускаются на эмулированном железе: " +
                    "NAND, SD-карта, сенсор, кнопки, звук и GPU") {}
        }
        Spacer(Modifier.height(8.dp))
    }
}

@Composable
private fun Divider() = HorizontalDivider(
    Modifier.padding(horizontal = 20.dp),
    color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.5f),
)

@Composable
fun LogScreen(state: AppState, which: String, share: () -> Unit) {
    val f = if (which == "qemu") java.io.File(state.st.dir, "qemu.log") else state.st.console
    val text = remember(which) {
        if (f.exists()) f.readText().takeLast(60000) else "Журнал пуст: эмулятор ещё не запускался."
    }
    Column(Modifier.verticalScroll(rememberScrollState())) {
        ScreenTitle(if (which == "qemu") "Журнал эмулятора" else "Журнал ядра", f.name)
        androidx.compose.material3.Surface(
            shape = androidx.compose.foundation.shape.RoundedCornerShape(20.dp),
            color = MaterialTheme.colorScheme.surfaceContainer,
            modifier = Modifier.padding(12.dp),
        ) {
            androidx.compose.foundation.text.selection.SelectionContainer {
                Text(text, style = MaterialTheme.typography.bodySmall.copy(
                    fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace),
                    modifier = Modifier.padding(16.dp))
            }
        }
        androidx.compose.material3.TextButton(share, Modifier.padding(start = 12.dp, bottom = 24.dp)) {
            Text("Поделиться журналами")
        }
    }
}
