package org.oldarmemu.hd2

import android.graphics.Bitmap
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.net.LocalSocket
import android.net.LocalSocketAddress
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteOrder
import java.nio.MappedByteBuffer
import java.nio.channels.FileChannel
import java.util.concurrent.LinkedBlockingQueue

/**
 * Shared memory written by the emulator (hw/arm/htcleo/leo_hostlink.c):
 * the HD2 screen as RGBA and a ring of s16 PCM sound.
 */
class Hostlink private constructor(private val buf: MappedByteBuffer) {
    val fbSeq: Int get() = buf.getInt(0x10)
    val lcdOn: Boolean get() = buf.getInt(0x14) != 0

    /** Copy the current frame (480x800 RGBA) into [bmp]. */
    fun copyFrame(bmp: Bitmap) {
        val d = buf.duplicate()
        d.position(HDR)
        d.limit(HDR + FB_W * FB_H * 4)
        bmp.copyPixelsFromBuffer(d)
    }

    val audioRate: Int get() = buf.getInt(0x18)
    val audioChannels: Int get() = buf.getInt(0x1c)
    val audioFormatSeq: Int get() = buf.getInt(0x2c)
    val audioWritten: Int get() = buf.getInt(0x20)
    var audioRead: Int
        get() = buf.getInt(0x24)
        set(v) { buf.putInt(0x24, v) }
    private val ringSize: Int get() = buf.getInt(0x28)

    /** Copy [n] ring bytes starting at counter [from] into [dst]. */
    fun readAudio(from: Int, dst: ByteArray, n: Int) {
        val size = ringSize
        val d = buf.duplicate()
        var done = 0
        while (done < n) {
            val at = Integer.remainderUnsigned(from + done, size)
            val chunk = minOf(n - done, size - at)
            d.position(HDR + FB_W * FB_H * 4 + at)
            d.get(dst, done, chunk)
            done += chunk
        }
    }

    val ringBytes: Int get() = ringSize

    companion object {
        const val FB_W = 480
        const val FB_H = 800
        private const val HDR = 0x1000
        private const val MAGIC = 0x4c324448

        /** Map [f] once the emulator has created and initialised it. */
        fun open(f: File): Hostlink? {
            if (!f.exists() || f.length() < HDR + FB_W * FB_H * 4) return null
            val m = RandomAccessFile(f, "rw").use {
                it.channel.map(FileChannel.MapMode.READ_WRITE, 0, it.length())
            }
            m.order(ByteOrder.LITTLE_ENDIAN)
            return if (m.getInt(0) == MAGIC) Hostlink(m) else null
        }
    }
}

/** Minimal QMP client: sends input events, ignores the replies. */
class QmpInput(private val sock: File) {
    private val queue = LinkedBlockingQueue<String>()
    @Volatile private var running = true
    private var socket: LocalSocket? = null

    fun start() {
        Thread({ run() }, "qmp").start()
    }

    private fun run() {
        val s = LocalSocket()
        val deadline = System.currentTimeMillis() + 20000
        while (running) {
            try {
                s.connect(LocalSocketAddress(sock.absolutePath, LocalSocketAddress.Namespace.FILESYSTEM))
                break
            } catch (e: Exception) {
                if (System.currentTimeMillis() > deadline) return
                Thread.sleep(100)
            }
        }
        socket = s
        // drain greeting/replies/events so QEMU never blocks on us
        Thread({
            try {
                val inp = s.inputStream
                val b = ByteArray(4096)
                while (inp.read(b) >= 0) { /* discard */ }
            } catch (_: Exception) { }
        }, "qmp-rx").start()
        try {
            val out = s.outputStream
            out.write("{\"execute\":\"qmp_capabilities\"}\n".toByteArray())
            while (running) {
                val cmd = queue.take()
                if (cmd.isEmpty()) break
                out.write(cmd.toByteArray())
                out.flush()
            }
        } catch (_: Exception) {
        } finally {
            try { s.close() } catch (_: Exception) { }
        }
    }

    private fun events(json: String) {
        queue.offer("{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[$json]}}\n")
    }

    /** Touch at screen pixel ([x], [y]) of the 480x800 panel. */
    fun touch(x: Float, y: Float, down: Boolean?) {
        val ax = (x.coerceIn(0f, 479f) * 0x7fff / 479).toInt()
        val ay = (y.coerceIn(0f, 799f) * 0x7fff / 799).toInt()
        val sb = StringBuilder()
        sb.append("{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$ax}},")
        sb.append("{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$ay}}")
        if (down != null) {
            sb.append(",{\"type\":\"btn\",\"data\":{\"down\":$down,\"button\":\"left\"}}")
        }
        events(sb.toString())
    }

    /** Any QMP command, e.g. {"execute":"system_reset"}. */
    fun command(json: String) {
        queue.offer(json + "\n")
    }

    fun key(qcode: String, down: Boolean) {
        events("{\"type\":\"key\",\"data\":{\"down\":$down,\"key\":{\"type\":\"qcode\",\"data\":\"$qcode\"}}}")
    }

    fun stop() {
        running = false
        queue.offer("")
        try { socket?.shutdownInput() } catch (_: Exception) { }
    }

    companion object {
        /* HD2 buttons as QEMU key codes (see htc_leo.c leo_qcode_to_key) */
        const val VOLUP = "pgup"
        const val VOLDOWN = "pgdn"
        const val WINDOWS = "f1"
        const val CALL = "f2"
        const val END = "f3"
        const val POWER = "f4"
        const val BACK = "esc"
        const val HOME = "home"
    }
}

/** Plays the guest's sound ring through an AudioTrack. */
class AudioOut(private val latencyMs: Int = 100, private val link: () -> Hostlink?) {
    @Volatile private var running = true

    fun start() {
        Thread({ run() }, "hd2-audio").apply { priority = Thread.MAX_PRIORITY }.start()
    }

    fun stop() {
        running = false
    }

    private fun makeTrack(rate: Int, ch: Int): AudioTrack {
        val mask = if (ch == 1) AudioFormat.CHANNEL_OUT_MONO else AudioFormat.CHANNEL_OUT_STEREO
        val min = AudioTrack.getMinBufferSize(rate, mask, AudioFormat.ENCODING_PCM_16BIT)
        return AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setAudioFormat(AudioFormat.Builder()
                .setSampleRate(rate).setChannelMask(mask)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
            .setBufferSizeInBytes(maxOf(min * 2, rate * ch * 2 * latencyMs / 1000))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .build().also { it.play() }
    }

    private fun run() {
        var track: AudioTrack? = null
        var fmtSeq = -1
        var rd = 0
        var cur: Hostlink? = null
        val chunk = ByteArray(16384)
        try {
            while (running) {
                val l = link()
                if (l == null) { Thread.sleep(50); continue }
                if (l !== cur) { cur = l; rd = l.audioWritten; fmtSeq = -1 }
                val rate = l.audioRate
                val ch = l.audioChannels
                if (l.audioFormatSeq != fmtSeq || track == null) {
                    fmtSeq = l.audioFormatSeq
                    track?.release()
                    track = makeTrack(rate, ch)
                }
                var avail = l.audioWritten - rd
                if (avail < 0 || avail > l.ringBytes) { rd = l.audioWritten; avail = 0 }
                // never let latency build up past ~2.5x the chosen buffer
                val maxLag = rate * ch * 2 * latencyMs * 5 / 2000
                if (avail > maxLag) {
                    rd += (avail - maxLag / 2) and 3.inv()
                    avail = l.audioWritten - rd
                }
                if (avail <= 0) { Thread.sleep(4); continue }
                val n = minOf(avail, chunk.size) and 3.inv()
                if (n == 0) { Thread.sleep(2); continue }
                l.readAudio(rd, chunk, n)
                rd += n
                l.audioRead = rd
                track!!.write(chunk, 0, n)
            }
        } catch (_: InterruptedException) {
        } finally {
            track?.release()
        }
    }
}
