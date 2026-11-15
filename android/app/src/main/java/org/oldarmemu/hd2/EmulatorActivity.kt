package org.oldarmemu.hd2

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.graphics.Canvas
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
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.TextView

/** Shows the emulated HD2: screen + hardware buttons. */
class EmulatorActivity : Activity() {
    private lateinit var st: Storage
    private lateinit var qemu: QemuProcess
    private var vnc: VncClient? = null
    private lateinit var screen: ScreenView
    private lateinit var status: TextView
    private val ui = Handler(Looper.getMainLooper())

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
        screen = ScreenView(this)
        frame.addView(screen, FrameLayout.LayoutParams(-1, -1))
        status = TextView(this).apply {
            setTextColor(Color.LTGRAY)
            textSize = 13f
            gravity = Gravity.CENTER
            text = "Запуск эмулятора…"
        }
        frame.addView(status, FrameLayout.LayoutParams(-1, -2, Gravity.CENTER))
        root.addView(frame, LinearLayout.LayoutParams(-1, 0, 1f))
        root.addView(hd2Buttons())
        setContentView(root)

        val boot = if (intent.getStringExtra("boot") == "recovery")
            QemuProcess.Boot.RECOVERY else QemuProcess.Boot.ANDROID
        status.text = "Подготовка SD-карты…"
        Thread {
            try {
                qemu.start(boot)
                ui.post { status.text = "Запуск эмулятора…"; connect() }
            } catch (e: Exception) {
                ui.post { status.text = "Не удалось запустить QEMU: ${e.message}" }
            }
        }.start()
    }

    private fun connect() {
        val client = VncClient(st.vncSocket,
            onFrame = { ui.post { status.visibility = View.GONE; screen.invalidate() } },
            onError = { msg -> ui.post { showExit(msg) } })
        vnc = client
        screen.client = client
        client.start()
    }

    private fun showExit(msg: String) {
        val code = qemu.exitCode()
        val log = java.io.File(st.dir, "qemu.log").takeIf { it.exists() }?.readText()?.takeLast(600) ?: ""
        status.visibility = View.VISIBLE
        status.text = if (code != null) "Эмулятор остановлен (код $code)\n$log" else "Нет связи с экраном: $msg"
    }

    private fun button(label: String, keysym: Int): Button = Button(this).apply {
        text = label
        textSize = 12f
        isAllCaps = false
        setOnTouchListener { v, ev ->
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> { vnc?.key(keysym, true); v.isPressed = true }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    vnc?.key(keysym, false); v.isPressed = false
                }
            }
            true
        }
    }

    /* volume rocker and power button live on the HD2's sides */
    private fun sideButtons(): View = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        val lp = LinearLayout.LayoutParams(0, -2, 1f)
        addView(button("Громк. +", VncClient.KEY_VOLUP), lp)
        addView(button("Громк. −", VncClient.KEY_VOLDOWN), lp)
        addView(button("Питание", VncClient.KEY_POWER), lp)
    }

    /* the five buttons under the HD2 screen */
    private fun hd2Buttons(): View = LinearLayout(this).apply {
        orientation = LinearLayout.HORIZONTAL
        val lp = LinearLayout.LayoutParams(0, -2, 1f)
        addView(button("Вызов", VncClient.KEY_CALL), lp)
        addView(button("Домой", VncClient.KEY_HOME), lp)
        addView(button("Windows", VncClient.KEY_WINDOWS), lp)
        addView(button("Назад", VncClient.KEY_BACK), lp)
        addView(button("Сброс", VncClient.KEY_END), lp)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        mapKey(keyCode)?.let { vnc?.key(it, true); return true }
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        mapKey(keyCode)?.let { vnc?.key(it, false); return true }
        return super.onKeyUp(keyCode, event)
    }

    private fun mapKey(code: Int): Int? = when (code) {
        KeyEvent.KEYCODE_BACK -> VncClient.KEY_BACK
        KeyEvent.KEYCODE_VOLUME_UP -> VncClient.KEY_VOLUP
        KeyEvent.KEYCODE_VOLUME_DOWN -> VncClient.KEY_VOLDOWN
        else -> null
    }

    override fun onDestroy() {
        vnc?.stop()
        qemu.stop()
        super.onDestroy()
    }

    /** Draws the guest frame buffer scaled to fit and forwards touches. */
    class ScreenView(ctx: Context) : View(ctx) {
        var client: VncClient? = null
        private val paint = Paint(Paint.FILTER_BITMAP_FLAG)
        private val m = Matrix()
        private val inv = Matrix()
        private val dst = RectF()

        override fun onDraw(c: Canvas) {
            c.drawColor(Color.BLACK)
            val vc = client ?: return
            val bmp = vc.bitmap ?: return
            val s = minOf(width.toFloat() / bmp.width, height.toFloat() / bmp.height)
            val dw = bmp.width * s; val dh = bmp.height * s
            dst.set((width - dw) / 2, (height - dh) / 2, (width + dw) / 2, (height + dh) / 2)
            m.setRectToRect(RectF(0f, 0f, bmp.width.toFloat(), bmp.height.toFloat()),
                dst, Matrix.ScaleToFit.FILL)
            m.invert(inv)
            c.drawBitmap(bmp, m, paint)
        }

        @SuppressLint("ClickableViewAccessibility")
        override fun onTouchEvent(e: MotionEvent): Boolean {
            val vc = client ?: return true
            val p = floatArrayOf(e.x, e.y)
            inv.mapPoints(p)
            val down = when (e.actionMasked) {
                MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> true
                else -> false
            }
            vc.pointer(p[0].toInt(), p[1].toInt(), down)
            return true
        }
    }
}
