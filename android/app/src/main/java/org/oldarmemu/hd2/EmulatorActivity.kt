@file:OptIn(ExperimentalMaterial3Api::class)

package org.oldarmemu.hd2

import android.annotation.SuppressLint
import android.content.ContentValues
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.RectF
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.MediaStore
import android.view.HapticFeedbackConstants
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBarsPadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Call
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.ListItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.TextButton
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.foundation.Image
import androidx.compose.foundation.layout.width
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.oldarmemu.hd2.ui.Hd2Icons
import org.oldarmemu.hd2.ui.Hd2Theme
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Shows the emulated HD2: screen, hardware buttons and sound. */
class EmulatorActivity : ComponentActivity() {
    private lateinit var st: Storage
    private lateinit var cfg: Settings
    private lateinit var qemu: QemuProcess
    private lateinit var screen: ScreenView
    private val ui = Handler(Looper.getMainLooper())

    @Volatile private var link: Hostlink? = null
    private var input: QmpInput? = null
    private var audio: AudioOut? = null
    private var generation = 0
    private var poweredOff = false

    private var status by mutableStateOf<String?>("Запуск эмулятора…")
    private var fps by mutableStateOf("")
    private var menu by mutableStateOf(false)
    private var snapDialog by mutableStateOf(false)
    private var snapBusy by mutableStateOf(false)
    private var snapVersion by mutableStateOf(0)
    private lateinit var snaps: Snapshots
    private var bootMode = QemuProcess.Boot.ANDROID

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        st = Storage(this)
        cfg = Settings(this)
        qemu = QemuProcess(this, st)
        snaps = Snapshots(st)
        if (cfg.keepScreenOn) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        WindowCompat.setDecorFitsSystemWindows(window, false)
        if (cfg.immersive) {
            WindowInsetsControllerCompat(window, window.decorView).apply {
                hide(WindowInsetsCompat.Type.systemBars())
                systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        }
        screen = ScreenView(this, cfg) { link }
        screen.onFirstFrame = { ui.post { status = null } }
        screen.input = { x, y, down -> input?.touch(x, y, down) }

        setContent { Hd2Theme(cfg) { EmulatorUi() } }

        if (cfg.sound) audio = AudioOut(cfg.audioLatencyMs) { link }.also { it.start() }
        ui.post(fpsTick)
        val mode = if (intent.getStringExtra("boot") == "recovery")
            QemuProcess.Boot.RECOVERY else QemuProcess.Boot.ANDROID
        val slot = intent.getIntExtra("snapshot", 0)
        if (slot > 0 && snaps.slot(slot).exists) boot(mode, snaps.slot(slot)) else boot(mode)
    }

    /* ---------------- UI ---------------- */

    @Composable
    private fun EmulatorUi() {
        Column(
            Modifier.fillMaxSize().background(androidx.compose.ui.graphics.Color(0xFF0E0F10))
                .then(if (cfg.immersive) Modifier else Modifier.windowInsetsPadding(WindowInsets.safeDrawing)),
        ) {
            if (cfg.sideButtons) {
                Row(
                    Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 6.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    HwButton(Hd2Icons.VolumeUp, "Громкость +", QmpInput.VOLUP, Modifier.weight(1f))
                    HwButton(Hd2Icons.VolumeDown, "Громкость −", QmpInput.VOLDOWN, Modifier.weight(1f))
                    HwButton(Hd2Icons.Power, "Питание", QmpInput.POWER, Modifier.weight(1f))
                    IconButton({ menu = true }) {
                        Icon(Icons.Filled.MoreVert, "Меню", tint = androidx.compose.ui.graphics.Color.LightGray)
                    }
                }
            }
            Box(Modifier.weight(1f).fillMaxWidth()) {
                AndroidView({ screen }, Modifier.fillMaxSize())
                status?.let {
                    Text(it, color = androidx.compose.ui.graphics.Color.LightGray,
                        style = MaterialTheme.typography.bodyMedium,
                        modifier = Modifier.align(Alignment.Center).padding(24.dp))
                }
                if (cfg.showFps && fps.isNotEmpty()) {
                    Surface(
                        shape = RoundedCornerShape(10.dp),
                        color = androidx.compose.ui.graphics.Color(0x99000000),
                        modifier = Modifier.align(Alignment.TopEnd).padding(8.dp),
                    ) {
                        Text(fps, color = androidx.compose.ui.graphics.Color(0xFF7BDA9D),
                            style = MaterialTheme.typography.labelMedium,
                            modifier = Modifier.padding(horizontal = 8.dp, vertical = 3.dp))
                    }
                }
                if (!cfg.sideButtons) {
                    IconButton({ menu = true }, Modifier.align(Alignment.TopStart).alpha(0.6f)) {
                        Icon(Icons.Filled.MoreVert, "Меню", tint = androidx.compose.ui.graphics.Color.White)
                    }
                }
            }
            if (cfg.bottomButtons) {
                Row(
                    Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 8.dp),
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    HwButton(Icons.Filled.Call, "Вызов", QmpInput.CALL, Modifier.weight(1f),
                        androidx.compose.ui.graphics.Color(0xFF3DDC84))
                    HwButton(Icons.Filled.Home, "Домой", QmpInput.HOME, Modifier.weight(1f))
                    HwButton(Hd2Icons.Windows, "Windows", QmpInput.WINDOWS, Modifier.weight(1f))
                    HwButton(Icons.AutoMirrored.Filled.ArrowBack, "Назад", QmpInput.BACK, Modifier.weight(1f))
                    HwButton(Hd2Icons.CallEnd, "Сброс", QmpInput.END, Modifier.weight(1f),
                        androidx.compose.ui.graphics.Color(0xFFE5534B))
                }
            }
        }
        if (menu) MenuSheet()
        if (snapDialog) SnapshotDialog()
    }

    /** One HD2 hardware button: pressed while the finger is down. */
    @Composable
    private fun HwButton(icon: ImageVector, label: String, qcode: String, modifier: Modifier,
                         tint: androidx.compose.ui.graphics.Color = androidx.compose.ui.graphics.Color(0xFFD5D8DB)) {
        val view = LocalView.current
        var pressed by androidx.compose.runtime.remember { mutableStateOf(false) }
        Surface(
            shape = RoundedCornerShape(16.dp),
            color = if (pressed) androidx.compose.ui.graphics.Color(0xFF3A3F44)
                    else androidx.compose.ui.graphics.Color(0xFF212427),
            modifier = modifier.height(cfg.buttonSize.dp).pointerInput(qcode) {
                detectTapGestures(onPress = {
                    pressed = true
                    if (cfg.haptics) view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                    pressDown(qcode)
                    tryAwaitRelease()
                    input?.key(qcode, false)
                    pressed = false
                })
            },
        ) {
            Box(contentAlignment = Alignment.Center) {
                Icon(icon, label, tint = tint, modifier = Modifier.size((cfg.buttonSize * 0.45f).dp))
            }
        }
    }

    private fun pressDown(qcode: String) {
        if (poweredOff && qcode == QmpInput.POWER) boot(QemuProcess.Boot.ANDROID)
        else input?.key(qcode, true)
    }

    @Composable
    private fun MenuSheet() {
        ModalBottomSheet(onDismissRequest = { menu = false }) {
            Column(Modifier.navigationBarsPadding().padding(bottom = 16.dp)) {
                MenuItem(Hd2Icons.Camera, "Скриншот", "Сохранить экран HD2 в Галерею") {
                    screenshot()
                }
                MenuItem(Hd2Icons.Download, "Снимки состояния",
                    "Сохранить телефон как есть и продолжить потом") {
                    snapVersion++
                    snapDialog = true
                }
                MenuItem(Hd2Icons.Power, "Меню питания HD2", "Долгое нажатие кнопки питания") {
                    input?.key(QmpInput.POWER, true)
                    ui.postDelayed({ input?.key(QmpInput.POWER, false) }, 1800)
                }
                MenuItem(Icons.Filled.Refresh, "Перезагрузить", "Аппаратный сброс телефона") {
                    input?.command("{\"execute\":\"system_reset\"}")
                }
                MenuItem(Icons.Filled.Close, "Закрыть эмулятор", "Телефон выключится без сохранения") {
                    finish()
                }
            }
        }
    }

    /** Three save slots: picture, date, size; save / load / delete. */
    @Composable
    private fun SnapshotDialog() {
        @Suppress("UNUSED_VARIABLE") val v = snapVersion
        AlertDialog(
            onDismissRequest = { if (!snapBusy) snapDialog = false },
            title = { Text("Снимки состояния") },
            text = {
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    for (slot in snaps.slots()) SnapshotRow(slot)
                    Text("Снимок хранит память, экран и диски телефона. " +
                        "Сохранение займёт несколько секунд.",
                        style = MaterialTheme.typography.bodySmall)
                }
            },
            confirmButton = {
                TextButton({ snapDialog = false }, enabled = !snapBusy) { Text("Закрыть") }
            },
        )
    }

    @Composable
    private fun SnapshotRow(slot: Snapshots.Slot) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            val pic = androidx.compose.runtime.remember(slot.time) { slot.preview() }
            Box(Modifier.size(48.dp, 80.dp).background(androidx.compose.ui.graphics.Color.Black,
                RoundedCornerShape(6.dp))) {
                pic?.let { Image(it.asImageBitmap(), null, Modifier.fillMaxSize()) }
            }
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text("Слот ${slot.index}", style = MaterialTheme.typography.titleSmall)
                Text(if (slot.exists) SimpleDateFormat("d MMM, HH:mm", Locale("ru"))
                        .format(Date(slot.time)) + " · " + formatSize(slot.bytes) else "Пусто",
                    style = MaterialTheme.typography.bodySmall)
                Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    FilledTonalButton({ saveSnapshot(slot.index) }, enabled = !snapBusy && link != null) {
                        Text("Сохранить")
                    }
                    if (slot.exists) {
                        OutlinedButton({ snapDialog = false; boot(bootMode, slot) }, enabled = !snapBusy) {
                            Text("Загрузить")
                        }
                    }
                }
            }
            if (slot.exists) {
                IconButton({ snaps.delete(slot.index); snapVersion++ }, enabled = !snapBusy) {
                    Icon(Icons.Filled.Delete, "Удалить")
                }
            }
        }
    }

    private fun saveSnapshot(i: Int) {
        snapBusy = true
        snapDialog = false
        val shot = screen.snapshot()
        val mode = if (bootMode == QemuProcess.Boot.RECOVERY) "recovery" else "android"
        status = "Сохранение снимка…"
        Thread {
            val err = runCatching {
                QmpClient.connect(st.qmpCtlSocket, 3000).use { q ->
                    snaps.save(i, q, mode, shot) { p ->
                        ui.post { status = "Сохранение снимка… ${(p * 100).toInt()}%" }
                    }
                }
            }.exceptionOrNull()
            ui.post {
                snapBusy = false
                status = null
                Toast.makeText(this, if (err == null) "Снимок сохранён в слот $i"
                    else "Не удалось сохранить: ${err.message}", Toast.LENGTH_LONG).show()
            }
        }.start()
    }

    private fun formatSize(b: Long) = if (b >= 1L shl 30) "%.1f ГБ".format(b / (1L shl 30).toDouble())
        else "${b shr 20} МБ"

    @Composable
    private fun MenuItem(icon: ImageVector, title: String, summary: String, action: () -> Unit) {
        ListItem(
            headlineContent = { Text(title) },
            supportingContent = { Text(summary) },
            leadingContent = { Icon(icon, null) },
            modifier = Modifier.clickable { menu = false; action() },
        )
    }

    /* ---------------- emulator ---------------- */

    private val fpsTick = object : Runnable {
        override fun run() {
            val n = screen.takeFrameCount()
            fps = if (link != null) "$n к/с" else ""
            ui.postDelayed(this, 1000)
        }
    }

    private fun boot(mode: QemuProcess.Boot, slot: Snapshots.Slot? = null) {
        poweredOff = false
        bootMode = if (slot != null && slot.boot == "recovery") QemuProcess.Boot.RECOVERY else mode
        val gen = ++generation
        input?.stop()
        input = null
        link = null
        screen.reset()
        status = "Подготовка SD-карты…"
        Thread {
            try {
                qemu.stop()
                if (slot != null) {
                    ui.post { if (gen == generation) status = "Восстановление снимка…" }
                    snaps.restoreDisks(slot) { p ->
                        ui.post { if (gen == generation) status = "Восстановление снимка… ${(p * 100).toInt()}%" }
                    }
                }
                qemu.start(bootMode, slot?.state)
                ui.post { if (gen == generation) status = "Запуск эмулятора…" }
                if (slot != null) resumeIncoming(gen)
                input = QmpInput(st.qmpSocket).also { it.start() }
                // map the shared screen/sound file as soon as QEMU made it
                val until = System.currentTimeMillis() + 20000
                while (gen == generation && link == null && System.currentTimeMillis() < until) {
                    link = Hostlink.open(st.hostlink)
                    if (link == null) Thread.sleep(50)
                }
                val ran = qemu.waitFor()
                if (ran) ui.post { if (gen == generation) showExit() }
            } catch (e: Exception) {
                ui.post { status = "Не удалось запустить QEMU: ${e.message}" }
            }
        }.start()
    }

    /* a saved state loads paused: resume it once QEMU has read it */
    private fun resumeIncoming(gen: Int) {
        runCatching {
            QmpClient.connect(st.qmpCtlSocket).use { q ->
                val until = System.currentTimeMillis() + 120000
                while (gen == generation && System.currentTimeMillis() < until) {
                    when (q.status()) {
                        "paused", "postmigrate" -> { q.cmd("cont"); return }
                        "running" -> return
                    }
                    Thread.sleep(50)
                }
            }
        }
    }

    private fun showExit() {
        link = null
        input?.stop()
        input = null
        // the guest rebooted or powered off: act like cLK / the power button
        when (qemu.takeRebootRequest()) {
            "android" -> { status = "Перезагрузка…"; boot(QemuProcess.Boot.ANDROID); return }
            "recovery" -> { status = "Перезагрузка в recovery…"; boot(QemuProcess.Boot.RECOVERY); return }
            "off" -> {
                poweredOff = true
                screen.reset()
                status = "Телефон выключен.\nНажмите «Питание», чтобы включить."
                return
            }
        }
        val code = qemu.exitCode()
        val log = File(st.dir, "qemu.log").takeIf { it.exists() }?.readText()?.takeLast(600) ?: ""
        status = "Эмулятор остановлен (код $code)\n$log"
    }

    private fun screenshot() {
        val bmp = screen.snapshot() ?: return
        val name = "HD2_" + SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date()) + ".png"
        val values = ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, name)
            put(MediaStore.Images.Media.MIME_TYPE, "image/png")
            put(MediaStore.Images.Media.RELATIVE_PATH, "Pictures/HD2")
        }
        val ok = runCatching {
            val uri = contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)!!
            contentResolver.openOutputStream(uri)!!.use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
        }.isSuccess
        Toast.makeText(this, if (ok) "Скриншот сохранён в Pictures/HD2" else "Не удалось сохранить",
            Toast.LENGTH_SHORT).show()
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        mapKey(keyCode)?.let { if (event.repeatCount == 0) input?.key(it, true); return true }
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        mapKey(keyCode)?.let { input?.key(it, false); return true }
        return super.onKeyUp(keyCode, event)
    }

    private fun mapKey(code: Int): String? = when (code) {
        KeyEvent.KEYCODE_BACK -> QmpInput.BACK
        KeyEvent.KEYCODE_VOLUME_UP -> if (cfg.volumeKeys) QmpInput.VOLUP else null
        KeyEvent.KEYCODE_VOLUME_DOWN -> if (cfg.volumeKeys) QmpInput.VOLDOWN else null
        else -> null
    }

    override fun onPause() {
        super.onPause()
        // the main screen shows this frame on the phone picture
        screen.snapshot()?.let { b ->
            Thread {
                runCatching {
                    File(st.dir, LAST_FRAME).outputStream().use {
                        b.compress(Bitmap.CompressFormat.PNG, 90, it)
                    }
                }
            }.start()
        }
    }

    override fun onDestroy() {
        generation++
        ui.removeCallbacks(fpsTick)
        audio?.stop()
        input?.stop()
        screen.stopRendering()
        qemu.stop()
        super.onDestroy()
    }

    companion object {
        const val LAST_FRAME = "last-frame.png"
    }

    /**
     * Draws the shared frame buffer with a hardware canvas whenever the
     * emulator published a new frame, and forwards touches.
     */
    class ScreenView(ctx: Context, private val cfg: Settings, private val link: () -> Hostlink?) :
        SurfaceView(ctx), SurfaceHolder.Callback {
        var onFirstFrame: (() -> Unit)? = null
        var input: ((Float, Float, Boolean?) -> Unit)? = null

        private val bmp = Bitmap.createBitmap(Hostlink.FB_W, Hostlink.FB_H, Bitmap.Config.ARGB_8888)
        private val paint = Paint().apply { isFilterBitmap = cfg.smooth }
        private val m = Matrix()
        private val inv = Matrix()
        @Volatile private var surfaceOk = false
        @Volatile private var running = true
        @Volatile private var redraw = true
        @Volatile private var shown = false
        @Volatile private var frames = 0
        @Volatile private var hasFrame = false
        private var lastSeq = Int.MIN_VALUE

        init {
            holder.addCallback(this)
            Thread({ loop() }, "hd2-screen").apply { priority = Thread.MAX_PRIORITY - 1 }.start()
        }

        fun reset() {
            shown = false
            redraw = true
            lastSeq = Int.MIN_VALUE
        }

        fun stopRendering() { running = false }

        fun takeFrameCount(): Int { val n = frames; frames = 0; return n }

        /** A copy of the current frame, null before the first one. */
        fun snapshot(): Bitmap? = if (hasFrame) synchronized(bmp) { bmp.copy(Bitmap.Config.ARGB_8888, false) } else null

        override fun surfaceCreated(h: SurfaceHolder) { surfaceOk = true; redraw = true }
        override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) {
            val fw = Hostlink.FB_W.toFloat()
            val fh = Hostlink.FB_H.toFloat()
            val dst = when (cfg.scaleMode) {
                Settings.SCALE_FILL -> RectF(0f, 0f, w.toFloat(), hh.toFloat())
                Settings.SCALE_INTEGER -> {
                    val k = maxOf(1, minOf(w / Hostlink.FB_W, hh / Hostlink.FB_H)).toFloat()
                    val k2 = if (w >= fw * k && hh >= fh * k) k else minOf(w / fw, hh / fh)
                    RectF((w - fw * k2) / 2, (hh - fh * k2) / 2, (w + fw * k2) / 2, (hh + fh * k2) / 2)
                }
                else -> {
                    val s = minOf(w / fw, hh / fh)
                    RectF((w - fw * s) / 2, (hh - fh * s) / 2, (w + fw * s) / 2, (hh + fh * s) / 2)
                }
            }
            synchronized(m) {
                m.setRectToRect(RectF(0f, 0f, fw, fh), dst, Matrix.ScaleToFit.FILL)
                m.invert(inv)
            }
            redraw = true
        }
        override fun surfaceDestroyed(h: SurfaceHolder) { surfaceOk = false }

        private fun loop() {
            while (running) {
                val l = link()
                if (!surfaceOk) { Thread.sleep(20); continue }
                var changed = false
                if (l != null) {
                    val seq = l.fbSeq
                    if (seq != lastSeq) {
                        lastSeq = seq
                        synchronized(bmp) { l.copyFrame(bmp) }
                        hasFrame = true
                        changed = true
                        frames++
                        if (!shown && l.lcdOn) { shown = true; onFirstFrame?.invoke() }
                    }
                } else if (lastSeq != Int.MIN_VALUE) {
                    synchronized(bmp) { bmp.eraseColor(Color.BLACK) }
                    lastSeq = Int.MIN_VALUE
                    changed = true
                }
                if (changed || redraw) {
                    redraw = false
                    draw()
                } else {
                    Thread.sleep(4)
                }
            }
        }

        private fun draw() {
            val c = try { holder.lockHardwareCanvas() } catch (e: Exception) { null } ?: return
            try {
                c.drawColor(Color.BLACK)
                synchronized(m) { synchronized(bmp) { c.drawBitmap(bmp, m, paint) } }
            } finally {
                holder.unlockCanvasAndPost(c)
            }
        }

        private var lastDown = false

        @SuppressLint("ClickableViewAccessibility")
        override fun onTouchEvent(e: MotionEvent): Boolean {
            val p = floatArrayOf(e.x, e.y)
            synchronized(m) { inv.mapPoints(p) }
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> { input?.invoke(p[0], p[1], true); lastDown = true }
                MotionEvent.ACTION_MOVE -> if (lastDown) input?.invoke(p[0], p[1], null)
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    input?.invoke(p[0], p[1], false); lastDown = false
                }
            }
            return true
        }
    }
}
