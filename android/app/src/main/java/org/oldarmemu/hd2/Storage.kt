package org.oldarmemu.hd2

import android.content.Context
import android.net.Uri
import android.provider.OpenableColumns
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
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
    val vncSocket = File(dir, "vnc.sock")
    val qmpSocket = File(dir, "qmp.sock")

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
     * Until the Adreno 200 GPU is emulated ROMs must use Android's software
     * GL renderer.  Build "zz-hd2emu-compat.zip" (flash it after the ROM):
     * it reuses the ROM's own update-binary and rewrites egl.cfg.
     */
    private fun makeCompatZip(rom: File) {
        val binary = try {
            ZipFile(rom).use { z ->
                val e = z.getEntry("META-INF/com/google/android/update-binary") ?: return
                z.getInputStream(e).readBytes()
            }
        } catch (e: Exception) {
            return
        }
        val out = File(sdcard, "zz-hd2emu-compat.zip")
        ZipOutputStream(out.outputStream()).use { z ->
            z.putNextEntry(ZipEntry("META-INF/com/google/android/update-binary"))
            z.write(binary); z.closeEntry()
            z.putNextEntry(ZipEntry("META-INF/com/google/android/updater-script"))
            z.write(COMPAT_SCRIPT.toByteArray()); z.closeEntry()
            z.putNextEntry(ZipEntry("egl.cfg"))
            z.write("0 0 android\n".toByteArray()); z.closeEntry()
        }
    }

    /**
     * (Re)build the SD card image when the set of files changed; otherwise
     * keep it, so whatever the guest wrote (backups, logs) survives.
     */
    fun prepareSdImage() {
        val files = sdcard.listFiles()?.filter { it.isFile }?.sortedBy { it.name } ?: emptyList()
        val stamp = files.joinToString("\n") { "${it.name}|${it.length()}|${it.lastModified()}" }
        if (sdImage.exists() && sdStamp.exists() && sdStamp.readText() == stamp) return
        FatImage.build(sdImage, files)
        sdStamp.writeText(stamp)
    }

    fun sdcardFiles(): List<String> = sdcard.listFiles()?.map { it.name }?.sorted() ?: emptyList()

    fun clearSdcard() {
        sdcard.listFiles()?.forEach { it.deleteRecursively() }
    }

    fun wipeNand() {
        nand.delete()
    }

    companion object {
        const val COMPAT_SCRIPT = """ui_print("HD2 emulator compatibility");
mount("yaffs2", "MTD", "system", "/system");
package_extract_file("egl.cfg", "/system/lib/egl/egl.cfg");
set_perm(0, 0, 0644, "/system/lib/egl/egl.cfg");
unmount("/system");
ui_print("software OpenGL ES enabled");
"""
    }
}
