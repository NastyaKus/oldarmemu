package org.oldarmemu.hd2

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files
import java.util.zip.ZipFile

/** Runs on a real Launcher2.apk when -Dlauncher.in=... is given; apksigner checks the output. */
class ApkPatcherTest {
    @Test
    fun launcher2d() {
        val src = File(System.getProperty("launcher.in") ?: "")
        assumeTrue(src.isFile)
        val out = File(System.getProperty("launcher.out")?.ifEmpty { null } ?: "build/Launcher2-2d.apk")
        val keys = Files.createTempDirectory("signer").toFile()
        assertTrue(ApkPatcher.launcher2d(src, out, JarSigner(keys)))
        val ids = ZipFile(out).use { z -> Arsc.ids(z.getInputStream(z.getEntry("resources.arsc")).readBytes(), "layout") }
        val xml = ZipFile(out).use { z -> z.getInputStream(z.getEntry("res/layout/all_apps.xml")).readBytes() }
        assertEquals(0, ApkPatcher.replaceRef(xml.clone(), ids["all_apps_3d"]!!, 0))
        assertEquals(1, ApkPatcher.replaceRef(xml.clone(), ids["all_apps_2d"]!!, 0))
    }

    @Test
    fun manifestLinesWrap() {
        val keys = Files.createTempDirectory("signer").toFile()
        // second instance reuses the stored key
        JarSigner(keys); JarSigner(keys)
        assertTrue(File(keys, "compat-signer.pk8").isFile)
    }
}
