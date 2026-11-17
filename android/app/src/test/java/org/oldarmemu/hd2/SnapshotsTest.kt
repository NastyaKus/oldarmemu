package org.oldarmemu.hd2

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test
import java.io.File

class SnapshotsTest {
    @Test
    fun sparseCopyKeepsDataAndSize() {
        val dir = createTempDir("snap")
        try {
            val src = File(dir, "nand.img")
            val mb = 1 shl 20
            val data = ByteArray(5 * mb + 1234)             // not a chunk multiple
            for (i in mb until mb + 4096) data[i] = (i * 7).toByte()     // chunk 1
            for (i in 4 * mb until data.size) data[i] = 0x5a            // chunk 4, tail
            src.writeBytes(data)
            val dst = File(dir, "copy.img")
            dst.writeBytes(ByteArray(10) { 1 })               // stale content
            var last = 0f
            Snapshots.sparseCopy(src, dst) { last = it }
            assertEquals(data.size.toLong(), dst.length())
            assertArrayEquals(data, dst.readBytes())
            assertEquals(1f, last)
        } finally {
            dir.deleteRecursively()
        }
    }
}
