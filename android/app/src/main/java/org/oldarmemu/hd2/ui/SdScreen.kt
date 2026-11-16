@file:OptIn(ExperimentalMaterial3Api::class)

package org.oldarmemu.hd2.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExtendedFloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import org.oldarmemu.hd2.Storage

@Composable
fun SdScreen(state: AppState, addToSd: () -> Unit) {
    @Suppress("UNUSED_VARIABLE") val v = state.version
    val files = state.st.sdcardEntries()
    var delete by remember { mutableStateOf<Storage.SdFile?>(null) }
    var clearAll by remember { mutableStateOf(false) }

    delete?.let { f ->
        ConfirmDialog("Удалить «${f.name}» с SD-карты?", "Удалить", { delete = null }) {
            state.st.deleteSdFile(f.name); state.refresh()
        }
    }
    if (clearAll) {
        ConfirmDialog("Удалить все файлы с виртуальной SD-карты?", "Очистить", { clearAll = false }) {
            state.st.clearSdcard(); state.refresh()
        }
    }

    Box(Modifier.fillMaxSize()) {
        LazyColumn(contentPadding = PaddingValues(bottom = 96.dp)) {
            item {
                ScreenTitle("SD-карта",
                    "${files.size} файл(ов) · ${formatSize(files.sumOf { it.size })} · " +
                        "образ ${state.cfg.sdSizeMb} МБ, FAT32")
            }
            if (files.isEmpty()) item {
                Column(Modifier.fillMaxWidth().padding(48.dp),
                    horizontalAlignment = Alignment.CenterHorizontally) {
                    Icon(Hd2Icons.SdCard, null, Modifier.size(72.dp),
                        tint = MaterialTheme.colorScheme.outline)
                    Spacer(Modifier.height(16.dp))
                    Text("Карта пуста.\nДобавьте zip прошивки, gapps и другие файлы — " +
                        "в recovery они будут в /sdcard.", textAlign = TextAlign.Center,
                        color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
            items(files, key = { it.name }) { f ->
                Surface(
                    shape = RoundedCornerShape(20.dp),
                    color = MaterialTheme.colorScheme.surfaceContainer,
                    modifier = Modifier.fillMaxWidth().padding(horizontal = 12.dp, vertical = 4.dp),
                ) {
                    ListItem(
                        headlineContent = { Text(f.name, maxLines = 2) },
                        supportingContent = {
                            Text(formatSize(f.size) +
                                if (f.generated) " · создан эмулятором, прошить после прошивки" else "")
                        },
                        leadingContent = {
                            IconBadge(if (f.name.endsWith(".zip", true)) Hd2Icons.SdCard else Hd2Icons.Doc,
                                if (f.generated) MaterialTheme.colorScheme.tertiary
                                else MaterialTheme.colorScheme.primary)
                        },
                        trailingContent = {
                            IconButton({ delete = f }) { Icon(Icons.Filled.Delete, "Удалить") }
                        },
                        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
                    )
                }
            }
            if (files.isNotEmpty()) item {
                Box(Modifier.fillMaxWidth().padding(top = 8.dp), contentAlignment = Alignment.Center) {
                    androidx.compose.material3.TextButton({ clearAll = true }) {
                        Text("Очистить всю карту")
                    }
                }
            }
        }
        ExtendedFloatingActionButton(
            onClick = addToSd,
            icon = { Icon(Icons.Filled.Add, null) },
            text = { Text("Добавить файлы") },
            modifier = Modifier.align(Alignment.BottomEnd).padding(20.dp),
        )
    }
}
