package org.oldarmemu.hd2.ui

import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Handler
import android.os.Looper
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import org.oldarmemu.hd2.EmulatorActivity
import org.oldarmemu.hd2.Settings
import org.oldarmemu.hd2.Snapshots
import org.oldarmemu.hd2.Storage
import java.io.File
import kotlin.concurrent.thread

/** State shared by the screens of the main activity. */
class AppState(val ctx: Context) {
    val st = Storage(ctx)
    val cfg = Settings(ctx)
    val snaps = Snapshots(st)
    private val main = Handler(Looper.getMainLooper())

    /** bumped after anything changed on disk: screens re-read it */
    var version by mutableIntStateOf(0)
        private set
    /** bumped when the theme settings changed */
    var themeVersion by mutableIntStateOf(0)

    var busy by mutableStateOf<String?>(null)
        private set
    var progress by mutableStateOf<Float?>(null)
        private set
    var message by mutableStateOf<String?>(null)

    fun refresh() { version++ }

    /** Run [block] off the UI thread with a progress dialog; it returns the result text. */
    fun run(title: String, block: (report: (Float?) -> Unit) -> String) {
        if (busy != null) return
        busy = title
        progress = null
        thread(name = "hd2-op") {
            val msg = try {
                block { p -> main.post { progress = p } }
            } catch (e: Exception) {
                "Ошибка: ${e.message ?: e.javaClass.simpleName}"
            }
            main.post {
                busy = null
                progress = null
                message = msg
                refresh()
            }
        }
    }

    fun launch(boot: String) {
        if (boot == "recovery" && !st.recovery.exists()) {
            message = "Сначала импортируйте recovery-образ (например, CWM для leo)."
            return
        }
        ctx.startActivity(Intent(ctx, EmulatorActivity::class.java)
            .putExtra("boot", boot).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }

    /** Resume the phone from snapshot slot [i]. */
    fun launchSnapshot(i: Int) {
        ctx.startActivity(Intent(ctx, EmulatorActivity::class.java)
            .putExtra("boot", "android").putExtra("snapshot", i)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    }

    /** The last frame the emulator showed, for the phone picture. */
    fun lastFrame(): Bitmap? {
        val f = File(st.dir, EmulatorActivity.LAST_FRAME)
        return if (f.exists()) runCatching { BitmapFactory.decodeFile(f.path) }.getOrNull() else null
    }

    val romOnSd: Boolean
        get() = st.sdcardEntries().any { it.name.endsWith(".zip", true) && !it.generated }
}

fun formatSize(bytes: Long): String = when {
    bytes >= 1L shl 30 -> "%.1f ГБ".format(bytes / (1L shl 30).toDouble())
    bytes >= 1L shl 20 -> "%.1f МБ".format(bytes / (1L shl 20).toDouble())
    bytes >= 1L shl 10 -> "${bytes shr 10} КБ"
    else -> "$bytes Б"
}

val Settings.themeLabel get() = when (theme) {
    Settings.THEME_DARK -> "Тёмная"
    Settings.THEME_LIGHT -> "Светлая"
    else -> "Как в системе"
}
