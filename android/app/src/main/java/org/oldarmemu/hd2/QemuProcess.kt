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

    fun start(boot: Boot, nandMb: Int = 512) {
        st.vncSocket.delete()
        st.qmpSocket.delete()
        val args = mutableListOf(
            binary.absolutePath,
            "-M", "htc-leo,nand-size=$nandMb" +
                if (boot == Boot.RECOVERY && !st.recovery.exists()) ",boot-mode=recovery" else "",
            "-accel", "tcg,tb-size=128",
            "-nodefaults",
            "-drive", "if=mtd,format=raw,file=${st.nand.absolutePath}",
            "-drive", "if=sd,format=raw,file=${st.sdImage.absolutePath}",
            "-display", "vnc=unix:${st.vncSocket.absolutePath}",
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

    fun exitCode(): Int? = process?.let { if (it.isAlive) null else it.exitValue() }
}
