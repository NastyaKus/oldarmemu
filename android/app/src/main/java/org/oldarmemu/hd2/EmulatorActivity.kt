package org.oldarmemu.hd2

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.RectF
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView

/** Shows the emulated HD2: screen + hardware buttons + sound. */
class EmulatorActivity : Activity() {
    private lateinit var st: Storage
    private lateinit var qemu: QemuProcess
    private lateinit var screen: ScreenView
    private lateinit var status: TextView
    private lateinit var fps: TextView
    private val ui = Handler(Looper.getMainLooper())

    @Volatile private var link: Hostlink? = null
    private var input: QmpInput? = null
    private var audio: AudioOut? = null
    private var generation = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        st = Storage(this)
        qemu = QemuProcess(this, st)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.rgb(24, 24, 24))
        }
        root.addView(sideButtons())
        val frame = FrameLayout(this)
        screen = ScreenView(this) { link }
        screen.onFirstFrame = { ui.post { status.visibility = View.GONE } }
        screen.input = { x, y, down -> input?.touch(x, y, down) }
        frame.addView(screen, FrameLayout.LayoutParams(-1, -1))
        status = TextView(this).apply {
            setTextColor(Color.LTGRAY)
            textSize = 13f
            gravity = Gravity.CENTER
            text = "Запуск эмулятора…"
        }
        frame.addView(status, FrameLayout.LayoutParams(-1, -2, Gravity.CENTER))
        fps = TextView(this).apply {
            setTextColor(Color.argb(160, 200, 200, 200))
            textSize = 10f
            setPadding(8, 4, 8, 4)
        }
        frame.addView(fps, FrameLayout.LayoutParams(-2, -2, Gravity.TOP or Gravity.END))
        root.addView(frame, LinearLayout.LayoutParams(-1, 0, 1f))
        root.addView(hd2Buttons())
        setContentView(root)

        audio = AudioOut { link }.also { it.start() }
        ui.post(fpsTick)
        boot(if (intent.getStringExtra("boot") == "recovery")
            QemuProcess.Boot.RECOVERY else QemuProcess.Boot.ANDROID)
    }

    private val fpsTick = object : Runnable {
        override fun run() {
            val n = screen.takeFrameCount()
            fps.text = if (link != null) "$n к/с" else ""
            ui.postDelayed(this, 1000)
        }
    }

    private var poweredOff = false

    private fun boot(mode: QemuProcess.Boot) {
        poweredOff = false
        val gen = ++generation
        input?.stop()
        input = null
        link = null
        screen.reset()
        status.visibility = View.VISIBLE
        status.text = "Подготовка SD-карты…"
        Thread {
            try {
                qemu.stop()
                qemu.start(mode)
                ui.post { if (gen == generation) status.text = "Запуск эмулятора…" }
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
                ui.post { status.visibility = View.VISIBLE; status.text = "Не удалось запустить QEMU: ${e.message}" }
            }
        }.start()
    }

    private fun showExit() {
        link = null
        input?.stop()
        input = null
        // the guest rebooted or powered off: act like cLK / the power button
        when (qemu.takeRebootRequest()) {
            "android" -> { status.visibility = View.VISIBLE; status.text = "Перезагрузка…"; boot(QemuProcess.Boot.ANDROID); return }
            "recovery" -> { status.visibility = View.VISIBLE; status.text = "Перезагрузка в recovery…"; boot(QemuProcess.Boot.RECOVERY); return }
            "off" -> {
                poweredOff = true
                screen.reset()
                status.visibility = View.VISIBLE
                status.text = "Телефон выключен.\nНажмите «Питание», чтобы включить."
                return
            }
        }
        val code = qemu.exitCode()
        val log = java.io.File(st.dir, "qemu.log").takeIf { it.exists() }?.readText()?.takeLast(600) ?: ""
        status.visibility = View.VISIBLE
        status.text = "Эмулятор остановлен (код $code)\n$log"
    }

    private fun button(label: String, qcode: String): Button = Button(this).apply {
        text = label
        textSize = 12f
        isAllCaps = false
        setOnTouchListener { v, ev ->
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    if (poweredOff && qcode == QmpInput.POWER) {
                        boot(QemuProcess.Boot.ANDROID)
                    } else {
                        input?.key(qcode, true)
                    }
                    v.isPressed = true
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    input?.key(qcode, false); v.isPressed = false
                }
            }
            true
        }
    }

    /* volume rocker and power button live on the HD2's sides */
    private fun sideButtons(): View = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        val lp = LinearLayout.LayoutParams(0, -2, 1f)
        addView(button("Громк. +", QmpInput.VOLUP), lp)
        addView(button("Громк. −", QmpInput.VOLDOWN), lp)
        addView(button("Питание", QmpInput.POWER), lp)
    }

    /* the five buttons under the HD2 screen */
    private fun hd2Buttons(): View = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        val lp = LinearLayout.LayoutParams(0, -2, 1f)
        addView(button("Вызов", QmpInput.CALL), lp)
        addView(button("Домой", QmpInput.HOME), lp)
        addView(button("Windows", QmpInput.WINDOWS), lp)
        addView(button("Назад", QmpInput.BACK), lp)
        addView(button("Сброс", QmpInput.END), lp)
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
        KeyEvent.KEYCODE_VOLUME_UP -> QmpInput.VOLUP
        KeyEvent.KEYCODE_VOLUME_DOWN -> QmpInput.VOLDOWN
        else -> null
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

    /**
     * Draws the shared frame buffer with a hardware canvas whenever the
     * emulator published a new frame, and forwards touches.
     */
    class ScreenView(ctx: Context, private val link: () -> Hostlink?) :
        SurfaceView(ctx), SurfaceHolder.Callback {
        var onFirstFrame: (() -> Unit)? = null
        var input: ((Float, Float, Boolean?) -> Unit)? = null

        private val bmp = Bitmap.createBitmap(Hostlink.FB_W, Hostlink.FB_H, Bitmap.Config.ARGB_8888)
        private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
        private val m = Matrix()
        private val inv = Matrix()
        @Volatile private var surfaceOk = false
        @Volatile private var running = true
        @Volatile private var redraw = true
        @Volatile private var shown = false
        @Volatile private var frames = 0
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

        override fun surfaceCreated(h: SurfaceHolder) { surfaceOk = true; redraw = true }
        override fun surfaceChanged(h: SurfaceHolder, f: Int, w: Int, hh: Int) {
            val s = minOf(w.toFloat() / Hostlink.FB_W, hh.toFloat() / Hostlink.FB_H)
            val dw = Hostlink.FB_W * s; val dh = Hostlink.FB_H * s
            synchronized(m) {
                m.setRectToRect(RectF(0f, 0f, Hostlink.FB_W.toFloat(), Hostlink.FB_H.toFloat()),
                    RectF((w - dw) / 2, (hh - dh) / 2, (w + dw) / 2, (hh + dh) / 2),
                    Matrix.ScaleToFit.FILL)
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
                        l.copyFrame(bmp)
                        changed = true
                        frames++
                        if (!shown && l.lcdOn) { shown = true; onFirstFrame?.invoke() }
                    }
                } else if (lastSeq != Int.MIN_VALUE) {
                    bmp.eraseColor(Color.BLACK)
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
                synchronized(m) { c.drawBitmap(bmp, m, paint) }
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
