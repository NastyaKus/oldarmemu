package org.oldarmemu.hd2

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.FileProvider
import org.oldarmemu.hd2.ui.Actions
import org.oldarmemu.hd2.ui.App
import org.oldarmemu.hd2.ui.AppState
import org.oldarmemu.hd2.ui.Hd2Theme
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Device screen: import a recovery and ROM zips, then boot into recovery
 * (to flash) or into the installed system from NAND, like a real HD2.
 */
class MainActivity : ComponentActivity() {
    private lateinit var state: AppState

    private val pickRecovery = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
        uri ?: return@registerForActivityResult
        state.run("Импорт recovery") { "Recovery импортирован: " + state.st.importRecovery(uri) }
    }

    private val pickSd = registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isEmpty()) return@registerForActivityResult
        state.run("Копирование на SD-карту") { report ->
            val names = uris.mapIndexed { i, u ->
                report(i.toFloat() / uris.size)
                state.st.addToSdcard(u)
            }
            "На SD-карту добавлено: " + names.joinToString()
        }
    }

    private val backup = registerForActivityResult(
        ActivityResultContracts.CreateDocument("application/zip")) { uri ->
        uri ?: return@registerForActivityResult
        state.run("Бэкап NAND") { report ->
            contentResolver.openOutputStream(uri)!!.use { state.st.backupNand(it) { p -> report(p) } }
            "Бэкап NAND сохранён."
        }
    }

    private val restore = registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        uri ?: return@registerForActivityResult
        state.run("Восстановление NAND") {
            contentResolver.openInputStream(uri)!!.use { state.st.restoreNand(it) { } }
            "NAND восстановлена: ${state.st.nandLayout().label}."
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge()
        super.onCreate(savedInstanceState)
        state = AppState(this)
        val act = Actions(
            importRecovery = { pickRecovery.launch(arrayOf("*/*")) },
            addToSd = { pickSd.launch(arrayOf("*/*")) },
            backupNand = {
                val stamp = SimpleDateFormat("yyyyMMdd-HHmm", Locale.US).format(Date())
                backup.launch("hd2-nand-$stamp.zip")
            },
            restoreNand = { restore.launch(arrayOf("application/zip", "*/*")) },
            shareLogs = { shareLogs() },
        )
        setContent {
            Hd2Theme(state.cfg, state.themeVersion) { App(state, act) }
        }
    }

    override fun onResume() {
        super.onResume()
        state.refresh()
    }

    private fun shareLogs() {
        val f = state.st.logsZip()
        val uri = FileProvider.getUriForFile(this, "$packageName.files", f)
        startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).apply {
            type = "application/zip"
            putExtra(Intent.EXTRA_STREAM, uri)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }, "Журналы HD2"))
    }
}
