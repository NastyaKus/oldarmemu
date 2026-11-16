package org.oldarmemu.hd2

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.util.zip.Deflater
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/** Files of the (single, for now) emulated HD2. */
class Storage(private val ctx: Context) {
    val dir: File = ctx.filesDir
    val nand = File(dir, "nand.img")
    val recovery = File(dir, "recovery.img")
    val sdcard = File(dir, "sdcard").apply { mkdirs() }
    val sdImage = File(dir, "sdcard.img")
    private val sdStamp = File(dir, "sdcard.stamp")
    val console = File(dir, "console.log")
    val hostlink = File(dir, "hostlink.shm")
    val qmpSocket = File(dir, "qmp.sock")
    private val nandCfg = File(dir, "nand.cfg")
    private val graphicsCfg = File(dir, "graphics.cfg")
    private val lastRom = File(dir, "last-rom.txt")

    /** NAND chip size and cLK partition sizes ("ppp"), all in MiB. */
    data class NandLayout(val sizeMb: Int, val systemMb: Int, val cacheMb: Int) {
        val label get() = "NAND $sizeMb МБ: system $systemMb МБ, cache $cacheMb МБ"
    }

    /**
     * Layout of the current NAND image.  Images made before the setting
     * existed used the HD2 EU layout; new ones get room for JB/KK ROMs.
     */
    fun nandLayout(): NandLayout {
        runCatching {
            val v = nandCfg.readText().trim().split(Regex("\\s+")).map { it.toInt() }
            if (v.size == 3) return NandLayout(v[0], v[1], v[2])
        }
        val l = if (nand.exists() && nand.length() > 0) LAYOUT_EU else LAYOUT_DEFAULT
        nandCfg.writeText("${l.sizeMb} ${l.systemMb} ${l.cacheMb}\n")
        return l
    }

    /** Re-partition: the NAND is erased (like changing the cLK ppp sizes). */
    fun setNandLayout(l: NandLayout) {
        nand.delete()
        nandCfg.writeText("${l.sizeMb} ${l.systemMb} ${l.cacheMb}\n")
    }

    /** true: the emulated Adreno 200; false: Android's software GL. */
    var gpu: Boolean
        get() = runCatching { graphicsCfg.readText().trim() != "soft" }.getOrDefault(true)
        set(v) {
            graphicsCfg.writeText(if (v) "gpu\n" else "soft\n")
            lastRom.takeIf { it.exists() }?.readText()?.trim()?.let { File(it) }
                ?.takeIf { it.exists() }?.let { makeCompatZip(it) }
        }

    fun displayName(uri: Uri): String {
        ctx.contentResolver.query(uri, null, null, null, null)?.use { c ->
            val i = c.getColumnIndex(OpenableColumns.DISPLAY_NAME)
            if (i >= 0 && c.moveToFirst()) return c.getString(i)
        }
        return uri.lastPathSegment?.substringAfterLast('/') ?: "file.bin"
    }

    private fun copy(uri: Uri, to: File) {
        ctx.contentResolver.openInputStream(uri)!!.use { input ->
            to.outputStream().use { input.copyTo(it, 1 shl 20) }
        }
    }

    /**
     * Import a recovery: either a raw Android boot image, or a flashable zip
     * (like recovery_5.0.2.6_leo_CWR.zip) that contains one.
     */
    fun importRecovery(uri: Uri): String {
        val tmp = File(ctx.cacheDir, "import.bin")
        copy(uri, tmp)
        try {
            if (isBootImage(tmp)) {
                tmp.copyTo(recovery, overwrite = true)
                return "recovery.img"
            }
            ZipFile(tmp).use { zip ->
                for (e in zip.entries()) {
                    if (e.isDirectory || !e.name.endsWith(".img")) continue
                    val out = File(ctx.cacheDir, "candidate.img")
                    zip.getInputStream(e).use { i -> out.outputStream().use { i.copyTo(it) } }
                    if (isBootImage(out)) {
                        out.copyTo(recovery, overwrite = true)
                        out.delete()
                        return e.name
                    }
                    out.delete()
                }
            }
            throw IllegalArgumentException("в файле нет Android boot image (ANDROID!)")
        } finally {
            tmp.delete()
        }
    }

    private fun isBootImage(f: File): Boolean {
        if (f.length() < 2048) return false
        val magic = ByteArray(8)
        f.inputStream().use { it.read(magic) }
        return String(magic, Charsets.US_ASCII) == "ANDROID!"
    }

    /** Put a file (ROM zip, gapps, ...) on the virtual SD card. */
    fun addToSdcard(uri: Uri): String {
        val name = displayName(uri).replace('/', '_')
        val dest = File(sdcard, name)
        copy(uri, dest)
        if (name.endsWith(".zip", true)) makeCompatZip(dest)
        return name
    }

    /**
     * Build "zz-hd2emu-compat.zip" (flash it after the ROM).  It reuses the
     * ROM's own edify update-binary (for AROMA installers the real one,
     * update-binary-installer), selects the GPU or software GL and removes
     * the MDP blitter HALs (copybit, hwcomposer), which the emulator lacks.
     */
    private fun makeCompatZip(rom: File) {
        val launcher = File(ctx.cacheDir, "Launcher2.apk")
        val launcher2d = File(ctx.cacheDir, "Launcher2-2d.apk")
        val binary = try {
            ZipFile(rom).use { z ->
                val e = z.getEntry("META-INF/com/google/android/update-binary-installer")
                    ?: z.getEntry("META-INF/com/google/android/update-binary") ?: return
                lastRom.writeText(rom.absolutePath)
                z.getEntry(LAUNCHER2)?.let { l ->
                    z.getInputStream(l).use { i -> launcher.outputStream().use { i.copyTo(it) } }
                }
                z.getInputStream(e).readBytes()
            }
        } catch (e: Exception) {
            return
        }
        val patched = try {
            launcher.exists() && ApkPatcher.launcher2d(launcher, launcher2d, JarSigner(dir))
        } catch (e: Exception) {
            false
        } finally {
            launcher.delete()
        }
        val out = File(sdcard, COMPAT)
        val gpu = this.gpu
        ZipOutputStream(out.outputStream()).use { z ->
            z.putNextEntry(ZipEntry("META-INF/com/google/android/update-binary"))
            z.write(binary); z.closeEntry()
            z.putNextEntry(ZipEntry("META-INF/com/google/android/updater-script"))
            z.write(compatScript(patched, gpu).toByteArray()); z.closeEntry()
            z.putNextEntry(ZipEntry("egl.cfg"))
            z.write((if (gpu) "0 0 android\n0 1 adreno200\n" else "0 0 android\n").toByteArray())
            z.closeEntry()
            if (patched) {
                z.putNextEntry(ZipEntry("Launcher2.apk"))
                launcher2d.inputStream().use { it.copyTo(z) }
                z.closeEntry()
            }
        }
        launcher2d.delete()
    }

    /**
     * (Re)build the SD card image when the set of files changed; otherwise
     * keep it, so whatever the guest wrote (backups, logs) survives.
     */
    fun prepareSdImage() {
        val files = sdcard.listFiles()?.filter { it.isFile }?.sortedBy { it.name } ?: emptyList()
        val sizeMb = Settings(ctx).sdSizeMb
        val stamp = "size=$sizeMb\n" +
            files.joinToString("\n") { "${it.name}|${it.length()}|${it.lastModified()}" }
        if (sdImage.exists() && sdStamp.exists() && sdStamp.readText() == stamp) return
        FatImage.build(sdImage, files, sizeMb.toLong() shl 20)
        sdStamp.writeText(stamp)
    }

    fun sdcardFiles(): List<String> = sdcard.listFiles()?.map { it.name }?.sorted() ?: emptyList()

    data class SdFile(val name: String, val size: Long, val generated: Boolean)

    fun sdcardEntries(): List<SdFile> =
        sdcard.listFiles()?.filter { it.isFile }?.sortedBy { it.name.lowercase() }
            ?.map { SdFile(it.name, it.length(), it.name == COMPAT) } ?: emptyList()

    fun deleteSdFile(name: String) {
        File(sdcard, name.replace('/', '_')).delete()
    }

    /** Something was flashed: the NAND image grew beyond the bootloader area. */
    val nandUsed: Boolean get() = nand.exists() && nand.length() > (8L shl 20)

    /**
     * Back up the NAND (image + partition layout) as a zip.  Erased flash
     * compresses to almost nothing, so this is much smaller than the image.
     */
    fun backupNand(out: OutputStream, progress: (Float) -> Unit) {
        nandLayout()
        ZipOutputStream(out.buffered(1 shl 20)).use { z ->
            z.setLevel(Deflater.BEST_SPEED)
            z.putNextEntry(ZipEntry("nand.cfg"))
            nandCfg.inputStream().use { it.copyTo(z) }
            z.closeEntry()
            z.putNextEntry(ZipEntry("nand.img"))
            val total = nand.length().coerceAtLeast(1)
            var done = 0L
            if (nand.exists()) nand.inputStream().use { inp ->
                val buf = ByteArray(1 shl 20)
                while (true) {
                    val n = inp.read(buf)
                    if (n < 0) break
                    z.write(buf, 0, n)
                    done += n
                    progress(done.toFloat() / total)
                }
            }
            z.closeEntry()
        }
    }

    /** Restore a backup made by [backupNand]; the current NAND is replaced. */
    fun restoreNand(inp: InputStream, progress: (Long) -> Unit) {
        val tmp = File(dir, "nand.img.restore")
        var cfg: String? = null
        var img = false
        ZipInputStream(inp.buffered(1 shl 20)).use { z ->
            while (true) {
                val e = z.nextEntry ?: break
                when (e.name) {
                    "nand.cfg" -> cfg = z.readBytes().toString(Charsets.US_ASCII)
                    "nand.img" -> {
                        img = true
                        tmp.outputStream().use { o ->
                            val buf = ByteArray(1 shl 20)
                            var done = 0L
                            while (true) {
                                val n = z.read(buf)
                                if (n < 0) break
                                o.write(buf, 0, n)
                                done += n
                                progress(done)
                            }
                        }
                    }
                }
            }
        }
        if (!img || cfg == null) {
            tmp.delete()
            throw IllegalArgumentException("это не бэкап NAND эмулятора HD2")
        }
        nand.delete()
        tmp.renameTo(nand)
        nandCfg.writeText(cfg!!)
    }

    /** Kernel and emulator logs zipped for sharing. */
    fun logsZip(): File {
        val d = File(ctx.cacheDir, "logs").apply { mkdirs() }
        val out = File(d, "hd2-logs.zip")
        ZipOutputStream(out.outputStream()).use { z ->
            for (f in listOf(console, File(dir, "qemu.log"))) {
                if (!f.exists()) continue
                z.putNextEntry(ZipEntry(f.name))
                f.inputStream().use { it.copyTo(z) }
                z.closeEntry()
            }
        }
        return out
    }

    fun clearSdcard() {
        sdcard.listFiles()?.forEach { it.deleteRecursively() }
    }

    fun wipeNand() {
        nand.delete()
    }

    fun hasCompatZip() = File(sdcard, COMPAT).exists()

    companion object {
        const val LAUNCHER2 = "system/app/Launcher2.apk"
        const val COMPAT = "zz-hd2emu-compat.zip"
        val LAYOUT_EU = NandLayout(512, 150, 44)
        val LAYOUT_DEFAULT = NandLayout(1024, 300, 44)
        val LAYOUTS = listOf(
            LAYOUT_EU,                      // HD2 EU, Gingerbread ROMs
            NandLayout(512, 250, 10),       // EU HD2 with a bigger ROM
            LAYOUT_DEFAULT,                 // HD2 T-Mobile US: ICS/JB ROMs
            NandLayout(1024, 400, 44),      // KitKat and large ROMs
        )

        fun compatScript(launcher2d: Boolean, gpu: Boolean = true) = buildString {
            append("""ui_print("HD2 emulator compatibility");
mount("yaffs2", "MTD", "system", "/system");
package_extract_file("egl.cfg", "/system/lib/egl/egl.cfg");
set_perm(0, 0, 0644, "/system/lib/egl/egl.cfg");
delete("/system/lib/hw/copybit.qsd8k.so", "/system/lib/hw/copybit.msm7k.so",
       "/system/lib/hw/hwcomposer.qsd8k.so", "/system/lib/hw/hwcomposer.msm7k.so",
       "/system/lib/hw/hwcomposer.default.so");
""")
            if (!gpu) append("""delete("/system/app/LiveWallpapers.apk", "/system/app/MagicSmokeWallpapers.apk",
       "/system/app/VisualizationWallpapers.apk");
""")
            if (launcher2d) append("""package_extract_file("Launcher2.apk", "/$LAUNCHER2");
set_perm(0, 0, 0644, "/$LAUNCHER2");
""")
            append("""unmount("/system");
""")
            append(if (gpu) "ui_print(\"OpenGL ES: emulated Adreno 200\");\n"
                   else """ui_print("OpenGL ES: software renderer");
ui_print("RenderScript live wallpapers removed (need OpenGL ES 2.0)");
""")
            append("ui_print(\"copybit and hwcomposer (MDP blitter) disabled\");\n")
            if (launcher2d) append("ui_print(\"Launcher2: 2D app drawer instead of RenderScript 3D\");\n")
        }
    }
}
