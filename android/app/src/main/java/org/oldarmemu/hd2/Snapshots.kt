package org.oldarmemu.hd2

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import org.json.JSONObject
import java.io.File
import java.io.RandomAccessFile

/**
 * Quick save/load of the running HD2.  A slot holds QEMU's device and RAM
 * state (a migration stream), copies of the NAND and SD images as they
 * were at that moment, the partition layout and a picture of the screen.
 */
class Snapshots(private val st: Storage) {
    data class Slot(
        val index: Int,
        val dir: File,
        val time: Long,
        val bytes: Long,
        val boot: String,
        val appVersion: Int,
    ) {
        val exists get() = time > 0
        fun preview(): Bitmap? = File(dir, SHOT).takeIf { it.exists() }
            ?.let { runCatching { BitmapFactory.decodeFile(it.path) }.getOrNull() }
        val state get() = File(dir, STATE)
    }

    fun slot(i: Int): Slot {
        val d = File(st.snapshotsDir, "slot$i")
        val meta = File(d, META)
        if (!meta.exists()) return Slot(i, d, 0, 0, "android", 0)
        return runCatching {
            val o = JSONObject(meta.readText())
            Slot(i, d, o.getLong("time"), d.listFiles()?.sumOf { it.length() } ?: 0,
                o.optString("boot", "android"), o.optInt("app", 0))
        }.getOrElse { Slot(i, d, 0, 0, "android", 0) }
    }

    fun slots() = (1..SLOTS).map { slot(it) }

    fun delete(i: Int) {
        File(st.snapshotsDir, "slot$i").deleteRecursively()
    }

    /**
     * Save the running emulator into slot [i]: QEMU is stopped while its
     * state and the disk images are written, then resumed.
     */
    fun save(i: Int, qmp: QmpClient, boot: String, shot: Bitmap?, progress: (Float) -> Unit) {
        val tmp = File(st.snapshotsDir, "slot$i.tmp")
        tmp.deleteRecursively()
        tmp.mkdirs()
        val wasRunning = qmp.status() == "running"
        qmp.cmd("stop")
        try {
            qmp.cmd("migrate", JSONObject().put("uri", "file:" + File(tmp, STATE).absolutePath))
            while (true) {
                val r = qmp.cmd("query-migrate")
                when (r.optString("status")) {
                    "completed" -> break
                    "failed", "cancelled" ->
                        throw IllegalStateException(r.optString("error-desc", "migration failed"))
                }
                Thread.sleep(50)
            }
            progress(0.3f)
            sparseCopy(st.nand, File(tmp, NAND)) { progress(0.3f + it * 0.4f) }
            sparseCopy(st.sdImage, File(tmp, SD)) { progress(0.7f + it * 0.3f) }
            st.nandCfg.takeIf { it.exists() }?.copyTo(File(tmp, CFG), true)
            st.sdStamp.takeIf { it.exists() }?.copyTo(File(tmp, STAMP), true)
        } finally {
            if (wasRunning) runCatching { qmp.cmd("cont") }
        }
        shot?.let { b -> File(tmp, SHOT).outputStream().use { b.compress(Bitmap.CompressFormat.PNG, 90, it) } }
        File(tmp, META).writeText(JSONObject()
            .put("time", System.currentTimeMillis())
            .put("boot", boot)
            .put("app", BuildConfig.VERSION_CODE).toString())
        val d = File(st.snapshotsDir, "slot$i")
        d.deleteRecursively()
        if (!tmp.renameTo(d)) throw IllegalStateException("cannot store slot $i")
    }

    /** Put the slot's disk images back; QEMU is then started with -incoming. */
    fun restoreDisks(slot: Slot, progress: (Float) -> Unit) {
        sparseCopy(File(slot.dir, NAND), st.nand) { progress(it * 0.6f) }
        sparseCopy(File(slot.dir, SD), st.sdImage) { progress(0.6f + it * 0.4f) }
        File(slot.dir, CFG).takeIf { it.exists() }?.copyTo(st.nandCfg, true)
        File(slot.dir, STAMP).takeIf { it.exists() }?.copyTo(st.sdStamp, true)
    }

    companion object {
        const val SLOTS = 3
        private const val STATE = "state.bin"
        private const val NAND = "nand.img"
        private const val SD = "sdcard.img"
        private const val CFG = "nand.cfg"
        private const val STAMP = "sdcard.stamp"
        private const val SHOT = "screen.png"
        private const val META = "meta.json"
        private const val CHUNK = 1 shl 20

        /**
         * Copy [src] to [dst] leaving all-zero 1 MiB chunks as holes: erased
         * NAND (stored inverted) and unused SD space are zero, so a slot
         * costs only what is actually in use.
         */
        fun sparseCopy(src: File, dst: File, progress: (Float) -> Unit = {}) {
            if (!src.exists()) {
                dst.delete()
                return
            }
            val size = src.length()
            dst.delete()
            RandomAccessFile(src, "r").use { inp ->
                RandomAccessFile(dst, "rw").use { out ->
                    out.setLength(size)
                    val buf = ByteArray(CHUNK)
                    var pos = 0L
                    var lastReport = 0L
                    while (pos < size) {
                        val n = minOf(CHUNK.toLong(), size - pos).toInt()
                        inp.readFully(buf, 0, n)
                        if (!isZero(buf, n)) {
                            out.seek(pos)
                            out.write(buf, 0, n)
                        }
                        pos += n
                        if (pos - lastReport >= 32L * CHUNK) {
                            lastReport = pos
                            progress(pos.toFloat() / size)
                        }
                    }
                }
            }
            progress(1f)
        }

        private fun isZero(b: ByteArray, n: Int): Boolean {
            for (i in 0 until n) if (b[i].toInt() != 0) return false
            return true
        }
    }
}
