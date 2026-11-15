package org.oldarmemu.hd2

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import kotlin.concurrent.thread

/**
 * Device screen: import a recovery and ROM zips, then boot into recovery
 * (to flash) or into the installed system from NAND - like a real HD2.
 */
class MainActivity : Activity() {
    private lateinit var st: Storage
    private lateinit var info: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        st = Storage(this)

        val col = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(32, 32, 32, 32)
        }
        col.addView(TextView(this).apply {
            text = "HTC HD2 (Leo) — эмулятор"
            textSize = 22f
        })
        info = TextView(this).apply {
            textSize = 14f
            setPadding(0, 16, 0, 16)
        }
        col.addView(info)

        fun add(label: String, action: () -> Unit) {
            col.addView(Button(this).apply {
                text = label
                isAllCaps = false
                setOnClickListener { action() }
            })
        }
        add("1. Импорт recovery (CWM/TWRP: .img или .zip)") { pick(REQ_RECOVERY, false) }
        add("2. Добавить файлы на SD-карту (прошивки, gapps)") { pick(REQ_SDCARD, true) }
        add("▶ Загрузить Recovery") { launch("recovery") }
        add("▶ Загрузить Android из NAND") { launch("android") }
        add("Очистить SD-карту") {
            confirm("Удалить все файлы с виртуальной SD-карты?") { st.clearSdcard(); refresh() }
        }
        add("Стереть NAND (как новый телефон)") {
            confirm("Стереть всю память NAND? Установленная прошивка пропадёт.") {
                st.wipeNand(); refresh()
            }
        }
        add("Журнал загрузки ядра") { showLog() }
        col.addView(TextView(this).apply {
            textSize = 12f
            setTextColor(Color.GRAY)
            setPadding(0, 24, 0, 0)
            text = HELP
        })
        setContentView(ScrollView(this).apply { addView(col) })
        refresh()
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun refresh() {
        val sd = st.sdcardFiles()
        val nandMb = st.nand.length() / (1 shl 20)
        info.text = buildString {
            append("Recovery: ")
            append(if (st.recovery.exists()) "есть (${st.recovery.length() / 1024} КБ)" else "нет")
            append("\nNAND: ")
            append(if (st.nand.exists() && st.nand.length() > 0) "$nandMb МБ образ" else "пустой")
            append("\nSD-карта: ")
            append(if (sd.isEmpty()) "пусто" else sd.joinToString(", "))
        }
    }

    private fun pick(req: Int, multiple: Boolean) {
        val i = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multiple)
        }
        startActivityForResult(i, req)
    }

    @Deprecated("platform API")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK || data == null) return
        val uris = mutableListOf<Uri>()
        data.clipData?.let { c -> for (k in 0 until c.itemCount) uris += c.getItemAt(k).uri }
        data.data?.let { if (uris.isEmpty()) uris += it }
        val wait = AlertDialog.Builder(this).setMessage("Копирование…").setCancelable(false).show()
        thread {
            val msg = try {
                when (requestCode) {
                    REQ_RECOVERY -> "Recovery импортирован: " + st.importRecovery(uris.first())
                    else -> "На SD-карту добавлено: " + uris.joinToString { st.addToSdcard(it) }
                }
            } catch (e: Exception) {
                "Ошибка: ${e.message}"
            }
            runOnUiThread {
                wait.dismiss()
                refresh()
                AlertDialog.Builder(this).setMessage(msg).setPositiveButton("OK", null).show()
            }
        }
    }

    private fun launch(boot: String) {
        if (boot == "recovery" && !st.recovery.exists()) {
            AlertDialog.Builder(this)
                .setMessage("Сначала импортируйте recovery-образ (например, CWM для leo).")
                .setPositiveButton("OK", null).show()
            return
        }
        startActivity(Intent(this, EmulatorActivity::class.java).putExtra("boot", boot))
    }

    private fun confirm(text: String, action: () -> Unit) {
        AlertDialog.Builder(this).setMessage(text)
            .setPositiveButton("Да") { _, _ -> action() }
            .setNegativeButton("Нет", null).show()
    }

    private fun showLog() {
        val log = (if (st.console.exists()) st.console.readText() else "журнал пуст").takeLast(20000)
        val tv = TextView(this).apply {
            text = log
            textSize = 10f
            setTextIsSelectable(true)
            setPadding(16, 16, 16, 16)
        }
        AlertDialog.Builder(this).setView(ScrollView(this).apply { addView(tv) })
            .setPositiveButton("Закрыть", null).show()
    }

    companion object {
        const val REQ_RECOVERY = 1
        const val REQ_SDCARD = 2
        const val HELP = """Как поставить прошивку, как на настоящий HD2:
1) импортируйте recovery для NAND (например recovery_5.0.2.6_leo_CWR.zip);
2) добавьте zip прошивки на SD-карту (рядом сам появится zz-hd2emu-compat.zip);
3) «Загрузить Recovery» → install zip from sdcard → прошивка, затем zz-hd2emu-compat.zip;
4) «Загрузить Android из NAND».
Кнопки HD2: Громк.± — навигация в recovery, «Вызов» — выбор, «Назад» — назад, «Питание» — разбудить экран."""
    }
}
