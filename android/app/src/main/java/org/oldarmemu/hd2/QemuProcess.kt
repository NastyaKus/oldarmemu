package org.oldarmemu.hd2

import android.content.Context
import android.net.ConnectivityManager
import java.io.File
import java.net.Inet4Address

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

    /** Start the emulator; with [incoming] it resumes that saved state (paused). */
    fun start(boot: Boot, incoming: File? = null) {
        val nl = st.nandLayout()
        val cfg = Settings(ctx)
        rebootFile.delete()
        st.hostlink.delete()
        st.qmpSocket.delete()
        st.qmpCtlSocket.delete()
        val args = mutableListOf(
            binary.absolutePath,
            "-L", dataDir().absolutePath,
            "-M", "htc-leo,nand-size=${nl.sizeMb},system-size=${nl.systemMb}" +
                ",cache-size=${nl.cacheMb},reboot-file=${rebootFile.absolutePath}" +
                ",hostlink=${st.hostlink.absolutePath},pin-cpu=${if (cfg.pinCpu) "on" else "off"}" +
                if (boot == Boot.RECOVERY && !st.recovery.exists()) ",boot-mode=recovery" else "",
            "-accel", "tcg,tb-size=${cfg.tbSize}",
            "-nodefaults",
            "-drive", "if=mtd,format=raw,file=${st.nand.absolutePath}",
            "-drive", "if=sd,format=raw,file=${st.sdImage.absolutePath}",
            "-display", "none",
            "-qmp", "unix:${st.qmpSocket.absolutePath},server=on,wait=off",
            "-qmp", "unix:${st.qmpCtlSocket.absolutePath},server=on,wait=off",
            "-serial", "file:${st.console.absolutePath}",
            "-monitor", "none",
        )
        if (boot == Boot.RECOVERY && st.recovery.exists()) {
            args += listOf("-kernel", st.recovery.absolutePath)
        }
        // Wi-Fi: the emulated BCM4329 sees one access point, "HD2-Emulator",
        // NATed through the phone's own connection (QEMU user networking)
        if (cfg.wifi) args += listOf("-netdev", "user,id=wifi")
        if (incoming != null) args += listOf("-incoming", "file:${incoming.absolutePath}")
        if (!st.nand.exists()) st.nand.createNewFile()   // grown on first use
        // a saved state comes with its own SD image: leave it untouched
        if (incoming == null) st.prepareSdImage()
        val pb = ProcessBuilder(args)
            .directory(st.dir)
            .redirectErrorStream(true)
            .redirectOutput(File(st.dir, "qemu.log"))
        pb.environment()["TMPDIR"] = ctx.cacheDir.absolutePath
        // Adreno 200 drawing: on the phone's GPU, or the software rasterizer
        pb.environment()["ADRENO_BACKEND"] = cfg.renderer
        if (cfg.pinCpu && cfg.vcpuPrime) pb.environment()["LEO_VCPU_PRIME"] = "1"
        if (cfg.ownCpu) {
            // our own ARMv7 engine; it reports its counters into qemu.log
            pb.environment()["LEO_JIT"] = "1"
            pb.environment()["LEO_JIT_STATS"] = "1"
        }
        if (cfg.wifi) pb.environment()["SLIRP_RESOLV_CONF"] = writeResolvConf().absolutePath
        process = pb.start()
    }

    /* Android has no /etc/resolv.conf: give the NAT the phone's DNS servers */
    private fun writeResolvConf(): File {
        val servers = mutableListOf<String>()
        try {
            val cm = ctx.getSystemService(ConnectivityManager::class.java)
            cm?.getLinkProperties(cm.activeNetwork)?.dnsServers?.forEach { a ->
                if (a is Inet4Address) a.hostAddress?.let { servers += it }
            }
        } catch (_: Exception) {
        }
        servers += listOf("8.8.8.8", "1.1.1.1")
        val f = File(st.dir, "resolv.conf")
        f.writeText(servers.distinct().take(3).joinToString("") { "nameserver $it\n" })
        return f
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
