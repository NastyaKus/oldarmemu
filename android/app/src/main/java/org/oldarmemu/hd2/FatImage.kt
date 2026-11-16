package org.oldarmemu.hd2

import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Builds a microSD card image: MBR + one FAT32 partition holding the given
 * files in the root directory (long file names, contiguous clusters).
 * The image is sparse and its size a power of two, as QEMU's SD card
 * model requires.
 */
object FatImage {
    private const val SECTOR = 512
    private const val PART_LBA = 2048L
    private const val RESERVED = 32
    private const val NFATS = 2

    fun build(out: File, files: List<File>, minSize: Long = 1L shl 30) {
        val payload = files.sumOf { roundUp(it.length(), 32768L) }
        var size = minSize
        while (size < payload * 5 / 4 + (64L shl 20)) size *= 2
        // FAT32 needs >= 65525 clusters
        val clusterSize = if (size <= (8L shl 30)) 4096 else 32768
        val spc = clusterSize / SECTOR

        val partSectors = size / SECTOR - PART_LBA
        // FAT size: solve clusters = (part - reserved - 2*fat) / spc
        var fatSectors = 1L
        while (true) {
            val clusters = (partSectors - RESERVED - NFATS * fatSectors) / spc
            val need = roundUp((clusters + 2) * 4, SECTOR.toLong()) / SECTOR
            if (need <= fatSectors) break
            fatSectors = need
        }
        val clusters = (partSectors - RESERVED - NFATS * fatSectors) / spc
        val dataLba = PART_LBA + RESERVED + NFATS * fatSectors

        out.delete()
        RandomAccessFile(out, "rw").use { raf ->
            raf.setLength(size)
            writeMbr(raf, partSectors)
            writeBootSectors(raf, partSectors, fatSectors, spc, clusters)

            // directory: one cluster per 1024 entries
            val dir = buildDirectory(files)
            val dirClusters = maxOf(1, roundUp(dir.size.toLong(), clusterSize.toLong()).toInt() / clusterSize)
            val fat = ByteBuffer.allocate(((2 + dirClusters + files.sumOf {
                roundUp(maxOf(it.length(), 1), clusterSize.toLong()) / clusterSize
            }) * 4).toInt()).order(ByteOrder.LITTLE_ENDIAN)
            fat.putInt(0, 0x0FFFFFF8); fat.putInt(4, 0x0FFFFFFF)
            var next = 2
            next = chain(fat, next, dirClusters)           // root dir at cluster 2

            // file contents, contiguous
            val starts = IntArray(files.size)
            for ((i, f) in files.withIndex()) {
                val n = (roundUp(maxOf(f.length(), 1), clusterSize.toLong()) / clusterSize).toInt()
                starts[i] = if (f.length() == 0L) 0 else next
                if (f.length() > 0) {
                    raf.seek((dataLba + (next - 2).toLong() * spc) * SECTOR)
                    f.inputStream().use { inp ->
                        val buf = ByteArray(1 shl 20)
                        while (true) {
                            val r = inp.read(buf); if (r < 0) break
                            raf.write(buf, 0, r)
                        }
                    }
                    next = chain(fat, next, n)
                }
            }
            patchStartClusters(dir, files, starts)
            raf.seek(dataLba * SECTOR)
            raf.write(dir)
            for (k in 0 until NFATS) {
                raf.seek((PART_LBA + RESERVED + k * fatSectors) * SECTOR)
                raf.write(fat.array(), 0, fat.position().coerceAtLeast(next * 4))
            }
            writeFsInfo(raf, clusters - (next - 2), next)
        }
    }

    private fun roundUp(v: Long, a: Long) = (v + a - 1) / a * a

    private fun chain(fat: ByteBuffer, first: Int, n: Int): Int {
        for (k in 0 until n) {
            val c = first + k
            fat.putInt(c * 4, if (k == n - 1) 0x0FFFFFFF else c + 1)
        }
        fat.position(maxOf(fat.position(), (first + n) * 4))
        return first + n
    }

    private fun writeMbr(raf: RandomAccessFile, partSectors: Long) {
        val b = ByteBuffer.allocate(SECTOR).order(ByteOrder.LITTLE_ENDIAN)
        val p = 446
        b.put(p, 0x00)
        b.put(p + 1, 0xFE.toByte()); b.put(p + 2, 0xFF.toByte()); b.put(p + 3, 0xFF.toByte())
        b.put(p + 4, 0x0C)                                  // FAT32 LBA
        b.put(p + 5, 0xFE.toByte()); b.put(p + 6, 0xFF.toByte()); b.put(p + 7, 0xFF.toByte())
        b.putInt(p + 8, PART_LBA.toInt())
        b.putInt(p + 12, partSectors.toInt())
        b.put(510, 0x55); b.put(511, 0xAA.toByte())
        raf.seek(0); raf.write(b.array())
    }

    private fun writeBootSectors(raf: RandomAccessFile, partSectors: Long,
                                 fatSectors: Long, spc: Int, clusters: Long) {
        val b = ByteBuffer.allocate(SECTOR).order(ByteOrder.LITTLE_ENDIAN)
        b.put(0, 0xEB.toByte()); b.put(1, 0x58); b.put(2, 0x90.toByte())
        "MSWIN4.1".toByteArray().copyInto(b.array(), 3)
        b.putShort(11, SECTOR.toShort())
        b.put(13, spc.toByte())
        b.putShort(14, RESERVED.toShort())
        b.put(16, NFATS.toByte())
        b.put(21, 0xF8.toByte())                           // media
        b.putShort(24, 63); b.putShort(26, 255)
        b.putInt(28, PART_LBA.toInt())                     // hidden sectors
        b.putInt(32, partSectors.toInt())
        b.putInt(36, fatSectors.toInt())
        b.putInt(44, 2)                                    // root cluster
        b.putShort(48, 1)                                  // FSInfo
        b.putShort(50, 6)                                  // backup boot
        b.put(64, 0x80.toByte()); b.put(66, 0x29)
        b.putInt(67, 0x48443253)                           // serial
        "HD2 SDCARD ".toByteArray().copyInto(b.array(), 71)
        "FAT32   ".toByteArray().copyInto(b.array(), 82)
        b.put(510, 0x55); b.put(511, 0xAA.toByte())
        for (lba in longArrayOf(0, 6)) {
            raf.seek((PART_LBA + lba) * SECTOR); raf.write(b.array())
        }
    }

    private fun writeFsInfo(raf: RandomAccessFile, free: Long, nextFree: Int) {
        val b = ByteBuffer.allocate(SECTOR).order(ByteOrder.LITTLE_ENDIAN)
        b.putInt(0, 0x41615252); b.putInt(484, 0x61417272)
        b.putInt(488, free.toInt()); b.putInt(492, nextFree)
        b.putInt(508, 0xAA550000.toInt())
        for (lba in longArrayOf(1, 7)) {
            raf.seek((PART_LBA + lba) * SECTOR); raf.write(b.array())
        }
    }

    /* ---- directory entries ---- */

    private val entryIndex = HashMap<Int, Int>()    // file index -> short entry offset

    private fun shortName(name: String, n: Int): ByteArray {
        val dot = name.lastIndexOf('.')
        val ext = (if (dot > 0) name.substring(dot + 1) else "")
            .uppercase().filter { it.isLetterOrDigit() }.take(3)
        val base = (if (dot > 0) name.substring(0, dot) else name)
            .uppercase().filter { it.isLetterOrDigit() }.take(6).ifEmpty { "FILE" }
        val tail = "~$n"
        val b = (base.take(8 - tail.length) + tail).padEnd(8)
        return (b + ext.padEnd(3)).toByteArray(Charsets.US_ASCII)
    }

    private fun lfnChecksum(sn: ByteArray): Int {
        var sum = 0
        for (c in sn) sum = (((sum and 1) shl 7) + (sum shr 1) + (c.toInt() and 0xff)) and 0xff
        return sum
    }

    private fun buildDirectory(files: List<File>): ByteArray {
        entryIndex.clear()
        val buf = java.io.ByteArrayOutputStream()
        // volume label
        val vol = ByteArray(32)
        "HD2 SDCARD ".toByteArray().copyInto(vol); vol[11] = 0x08
        buf.write(vol)
        for ((i, f) in files.withIndex()) {
            val sn = shortName(f.name, i + 1)
            val sum = lfnChecksum(sn)
            val units = f.name.toCharArray()
            val nParts = (units.size + 12) / 13
            for (p in nParts downTo 1) {
                val e = ByteArray(32)
                e[0] = (p or (if (p == nParts) 0x40 else 0)).toByte()
                e[11] = 0x0F; e[13] = sum.toByte()
                val offs = intArrayOf(1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)
                for (k in 0 until 13) {
                    val idx = (p - 1) * 13 + k
                    val ch = when {
                        idx < units.size -> units[idx].code
                        idx == units.size -> 0
                        else -> 0xFFFF
                    }
                    e[offs[k]] = (ch and 0xff).toByte(); e[offs[k] + 1] = (ch shr 8).toByte()
                }
                buf.write(e)
            }
            val e = ByteArray(32)
            sn.copyInto(e)
            e[11] = 0x20                                   // archive
            // 2011-01-01 12:00
            val time = (12 shl 11); val date = ((2011 - 1980) shl 9) or (1 shl 5) or 1
            for (o in intArrayOf(14, 22)) { e[o] = time.toByte(); e[o + 1] = (time shr 8).toByte() }
            for (o in intArrayOf(16, 18, 24)) { e[o] = date.toByte(); e[o + 1] = (date shr 8).toByte() }
            val len = f.length()
            e[28] = len.toByte(); e[29] = (len shr 8).toByte()
            e[30] = (len shr 16).toByte(); e[31] = (len shr 24).toByte()
            entryIndex[i] = buf.size()
            buf.write(e)
        }
        return buf.toByteArray()
    }

    private fun patchStartClusters(dir: ByteArray, files: List<File>, starts: IntArray) {
        for (i in files.indices) {
            val o = entryIndex[i] ?: continue
            val c = starts[i]
            dir[o + 26] = c.toByte(); dir[o + 27] = (c shr 8).toByte()
            dir[o + 20] = (c shr 16).toByte(); dir[o + 21] = (c shr 24).toByte()
        }
    }
}
