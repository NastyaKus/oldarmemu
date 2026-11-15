package org.oldarmemu.hd2

import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/** Writes build/fat-test.img; tools/android/check-fat.sh validates it. */
class FatImageTest {
    @Test
    fun buildsImage() {
        val dir = File(System.getProperty("fat.src") ?: "build/fat-src")
        dir.mkdirs()
        if (dir.listFiles().isNullOrEmpty()) {
            File(dir, "hello world.txt").writeText("hello from the HD2 emulator\n")
            File(dir, "a-very-long-file-name-for-lfn-check.bin").writeBytes(ByteArray(100000) { it.toByte() })
        }
        val out = File(System.getProperty("fat.out") ?: "build/fat-test.img")
        FatImage.build(out, dir.listFiles()!!.filter { it.isFile }.sortedBy { it.name })
        assertTrue(out.length() >= 1L shl 30)
    }
}
