package org.oldarmemu.hd2

import android.content.Context
import java.io.File

/** Runs the bundled qemu-system-arm (htc-leo machine) as a child process. */
class QemuProcess(private val ctx: Context, private val st: Storage) {
    enum class Boot { ANDROID, RECOVERY }

    private var process: Process? = null

    val running: Boolean get() = process?.isAlive == true

    private val binary: File
        get() = File(ctx.applicationInfo.nativeLibraryDir, "libqemu-leo.so")

    /* data directory for QEMU (-L): files bundled in assets/qemu */
    private fun dataDir(): File {
        val dir = File(st.dir, "qemu-data")
        for (name in listOf("keymaps/en-us")) {
            val f = File(dir, name)
            if (f.exists()) continue
            f.parentFile!!.mkdirs()
            ctx.assets.open("qemu/$name").use { i -> f.outputStream().use { i.copyTo(it) } }
        }
        return dir
    }

    private val rebootFile get() = File(st.dir, "reboot.txt")

    /** "android", "recovery" or "off" if the guest asked for it, else null. */
    fun takeRebootRequest(): String? {
        val f = rebootFile
        if (process?.isAlive == true || !f.exists()) return null
        return f.readText().trim().also { f.delete() }
    }

    fun start(boot: Boot, nandMb: Int = 512) {
        rebootFile.delete()
        st.hostlink.delete()
        st.qmpSocket.delete()
        val args = mutableListOf(
            binary.absolutePath,
            "-L", dataDir().absolutePath,
            "-M", "htc-leo,nand-size=$nandMb,reboot-file=${rebootFile.absolutePath}" +
                ",hostlink=${st.hostlink.absolutePath},pin-cpu=on" +
                if (boot == Boot.RECOVERY && !st.recovery.exists()) ",boot-mode=recovery" else "",
            "-accel", "tcg,tb-size=256",
            "-nodefaults",
            "-drive", "if=mtd,format=raw,file=${st.nand.absolutePath}",
            "-drive", "if=sd,format=raw,file=${st.sdImage.absolutePath}",
            "-display", "none",
            "-qmp", "unix:${st.qmpSocket.absolutePath},server=on,wait=off",
            "-serial", "file:${st.console.absolutePath}",
            "-monitor", "none",
        )
        if (boot == Boot.RECOVERY && st.recovery.exists()) {
            args += listOf("-kernel", st.recovery.absolutePath)
        }
        if (!st.nand.exists()) st.nand.createNewFile()   // grown on first use
        st.prepareSdImage()
        val pb = ProcessBuilder(args)
            .directory(st.dir)
            .redirectErrorStream(true)
            .redirectOutput(File(st.dir, "qemu.log"))
        pb.environment()["TMPDIR"] = ctx.cacheDir.absolutePath
        process = pb.start()
    }

    fun stop() {
        process?.let {
            it.destroy()
            if (!it.waitFor(2, java.util.concurrent.TimeUnit.SECONDS)) it.destroyForcibly()
        }
        process = null
    }

    /** Block until the current QEMU process ends; false if there is none. */
    fun waitFor(): Boolean {
        val p = process ?: return false
        p.waitFor()
        return true
    }

    fun waitExit(ms: Long) {
        process?.waitFor(ms, java.util.concurrent.TimeUnit.MILLISECONDS)
    }

    fun exitCode(): Int? = process?.let { if (it.isAlive) null else it.exitValue() }
}
