@file:OptIn(ExperimentalMaterial3Api::class)

package org.oldarmemu.hd2.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import org.oldarmemu.hd2.Storage

@Composable
fun ScreenTitle(title: String, subtitle: String? = null) {
    Column(Modifier.padding(start = 20.dp, end = 20.dp, top = 20.dp, bottom = 8.dp)) {
        Text(title, style = MaterialTheme.typography.displaySmall)
        if (subtitle != null) {
            Spacer(Modifier.height(4.dp))
            Text(subtitle, style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
fun SectionHeader(text: String) {
    Text(text, style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(start = 20.dp, top = 20.dp, bottom = 4.dp))
}

/** Rounded group of settings rows, as in the system Settings app. */
@Composable
fun SettingsGroup(content: @Composable () -> Unit) {
    Surface(
        shape = RoundedCornerShape(24.dp),
        color = MaterialTheme.colorScheme.surfaceContainer,
        modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp),
    ) { Column { content() } }
}

@Composable
fun IconBadge(icon: ImageVector?, tint: Color = MaterialTheme.colorScheme.primary) {
    if (icon == null) {         // keep rows aligned
        Spacer(Modifier.size(38.dp))
        return
    }
    Surface(shape = RoundedCornerShape(12.dp), color = tint.copy(alpha = 0.14f)) {
        Icon(icon, null, tint = tint, modifier = Modifier.padding(8.dp).size(22.dp))
    }
}

private val rowColors @Composable get() =
    ListItemDefaults.colors(containerColor = Color.Transparent)

@Composable
fun ClickRow(icon: ImageVector?, title: String, summary: String? = null,
             onClick: () -> Unit) {
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = summary?.let { { Text(it) } },
        leadingContent = { IconBadge(icon) },
        colors = rowColors,
        modifier = Modifier.clickable(onClick = onClick),
    )
}

@Composable
fun SwitchRow(icon: ImageVector?, title: String, summary: String? = null,
              initial: Boolean, onChange: (Boolean) -> Unit) {
    var on by remember { mutableStateOf(initial) }
    ListItem(
        headlineContent = { Text(title) },
        supportingContent = summary?.let { { Text(it) } },
        leadingContent = { IconBadge(icon) },
        trailingContent = { Switch(checked = on, onCheckedChange = null) },
        colors = rowColors,
        modifier = Modifier.selectable(selected = on, role = Role.Switch) {
            on = !on
            onChange(on)
        },
    )
}

@Composable
fun <T> ChoiceRow(icon: ImageVector?, title: String, options: List<T>, label: (T) -> String,
                  initial: T, summary: String? = null, onChange: (T) -> Unit) {
    var cur by remember { mutableStateOf(initial) }
    Column(Modifier.padding(bottom = 12.dp)) {
        ListItem(
            headlineContent = { Text(title) },
            supportingContent = summary?.let { { Text(it) } },
            leadingContent = { IconBadge(icon) },
            colors = rowColors,
        )
        SingleChoiceSegmentedButtonRow(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
            options.forEachIndexed { i, o ->
                SegmentedButton(
                    selected = o == cur,
                    onClick = { cur = o; onChange(o) },
                    shape = SegmentedButtonDefaults.itemShape(i, options.size),
                ) { Text(label(o), maxLines = 1) }
            }
        }
    }
}

@Composable
fun SliderRow(icon: ImageVector?, title: String, range: IntRange, step: Int, initial: Int,
              unit: String, onChange: (Int) -> Unit) {
    var v by remember { mutableFloatStateOf(initial.toFloat()) }
    Column(Modifier.padding(bottom = 8.dp)) {
        ListItem(
            headlineContent = { Text(title) },
            supportingContent = { Text("${v.toInt()} $unit") },
            leadingContent = { IconBadge(icon) },
            colors = rowColors,
        )
        Slider(
            value = v,
            onValueChange = { v = (Math.round(it / step) * step).toFloat() },
            onValueChangeFinished = { onChange(v.toInt()) },
            valueRange = range.first.toFloat()..range.last.toFloat(),
            steps = (range.last - range.first) / step - 1,
            modifier = Modifier.padding(horizontal = 20.dp),
        )
    }
}

@Composable
fun BusyDialog(state: AppState) {
    val title = state.busy ?: return
    AlertDialog(
        onDismissRequest = {},
        confirmButton = {},
        title = { Text(title) },
        text = {
            val p = state.progress
            if (p == null) {
                Row(verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    CircularProgressIndicator(Modifier.size(28.dp))
                    Text("Подождите…")
                }
            } else {
                Column {
                    LinearProgressIndicator(progress = { p }, modifier = Modifier.fillMaxWidth())
                    Spacer(Modifier.height(8.dp))
                    Text("${(p * 100).toInt()} %")
                }
            }
        },
    )
}

@Composable
fun MessageDialog(state: AppState) {
    val msg = state.message ?: return
    AlertDialog(
        onDismissRequest = { state.message = null },
        confirmButton = { TextButton({ state.message = null }) { Text("OK") } },
        text = { Text(msg) },
    )
}

@Composable
fun ConfirmDialog(text: String, confirm: String = "Да", onDismiss: () -> Unit, onConfirm: () -> Unit) {
    AlertDialog(
        onDismissRequest = onDismiss,
        confirmButton = { TextButton({ onDismiss(); onConfirm() }) { Text(confirm) } },
        dismissButton = { TextButton(onDismiss) { Text("Отмена") } },
        text = { Text(text) },
    )
}

/** Pick a NAND partition layout; changing it erases the NAND. */
@Composable
fun NandLayoutDialog(state: AppState, onDismiss: () -> Unit) {
    val cur = state.st.nandLayout()
    var pick by remember { mutableStateOf(cur) }
    var confirm by remember { mutableStateOf(false) }
    val hints = mapOf(
        Storage.LAYOUTS[0] to "HD2 EU, прошивки Gingerbread",
        Storage.LAYOUTS[1] to "HD2 EU, крупные прошивки",
        Storage.LAYOUTS[2] to "HD2 T-Mobile: ICS и JellyBean",
        Storage.LAYOUTS[3] to "KitKat и самые большие прошивки",
    )
    if (confirm) {
        ConfirmDialog(
            "Новая разметка: ${pick.label}.\n\nNAND будет стёрта, прошивку нужно будет " +
                "поставить заново. Recovery и SD-карта останутся.",
            confirm = "Стереть и применить",
            onDismiss = { confirm = false; onDismiss() },
        ) { state.st.setNandLayout(pick); state.refresh() }
        return
    }
    AlertDialog(
        onDismissRequest = onDismiss,
        icon = { Icon(Hd2Icons.Memory, null) },
        title = { Text("Разметка NAND") },
        text = {
            Column {
                for (l in Storage.LAYOUTS) {
                    Row(
                        Modifier.fillMaxWidth().clip12().selectable(pick == l, role = Role.RadioButton) { pick = l }
                            .padding(vertical = 6.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(selected = pick == l, onClick = null)
                        Column(Modifier.padding(start = 12.dp)) {
                            Text("${l.sizeMb} МБ · system ${l.systemMb} · cache ${l.cacheMb}",
                                style = MaterialTheme.typography.bodyLarge)
                            Text(hints[l] ?: "", style = MaterialTheme.typography.bodySmall,
                                color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(enabled = pick != cur, onClick = { confirm = true }) { Text("Применить") }
        },
        dismissButton = { TextButton(onDismiss) { Text("Отмена") } },
    )
}

private fun Modifier.clip12() = this.clip(RoundedCornerShape(12.dp))
