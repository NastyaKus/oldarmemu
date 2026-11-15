package org.oldarmemu.hd2

import android.graphics.Bitmap
import android.net.LocalSocket
import android.net.LocalSocketAddress
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File

/**
 * Minimal RFB 3.8 client for QEMU's VNC server on a unix socket:
 * security "None", raw encoding, DesktopSize.  Frames land in [bitmap].
 */
class VncClient(
    private val socketPath: File,
    private val onFrame: () -> Unit,
    private val onError: (String) -> Unit,
) {
    @Volatile var bitmap: Bitmap? = null
        private set
    @Volatile var width = 480
        private set
    @Volatile var height = 800
        private set

    private var sock: LocalSocket? = null
    private var out: DataOutputStream? = null
    private var thread: Thread? = null
    @Volatile private var stopped = false
    private var pixels = IntArray(0)

    fun start() {
        thread = Thread({ run() }, "vnc").apply { start() }
    }

    fun stop() {
        stopped = true
        try { sock?.close() } catch (_: Exception) {}
    }

    private fun connect(): LocalSocket {
        val deadline = System.currentTimeMillis() + 15000
        while (true) {
            try {
                val s = LocalSocket()
                s.connect(LocalSocketAddress(socketPath.absolutePath,
                    LocalSocketAddress.Namespace.FILESYSTEM))
                return s
            } catch (e: Exception) {
                if (System.currentTimeMillis() > deadline || stopped) throw e
                Thread.sleep(200)
            }
        }
    }

    private fun run() {
        try {
            val s = connect()
            sock = s
            val inp = DataInputStream(s.inputStream.buffered(1 shl 16))
            val o = DataOutputStream(s.outputStream.buffered())
            out = o
            handshake(inp, o)
            requestUpdate(false)
            while (!stopped) {
                when (inp.readUnsignedByte()) {
                    0 -> readUpdate(inp)
                    1 -> {      // SetColourMapEntries
                        inp.readUnsignedByte(); inp.readUnsignedShort()
                        val n = inp.readUnsignedShort(); inp.skipBytes(n * 6)
                    }
                    2 -> {}     // Bell
                    3 -> {      // ServerCutText
                        inp.skipBytes(3); inp.skipBytes(inp.readInt())
                    }
                    else -> throw IllegalStateException("unknown VNC message")
                }
            }
        } catch (e: Exception) {
            if (!stopped) onError(e.message ?: e.toString())
        }
    }

    private fun handshake(i: DataInputStream, o: DataOutputStream) {
        val ver = ByteArray(12); i.readFully(ver)
        o.write("RFB 003.008\n".toByteArray()); o.flush()
        val n = i.readUnsignedByte()
        if (n == 0) throw IllegalStateException("VNC refused connection")
        val types = ByteArray(n); i.readFully(types)
        o.writeByte(1); o.flush()                    // security: None
        if (i.readInt() != 0) throw IllegalStateException("VNC security failed")
        o.writeByte(1); o.flush()                    // shared
        setSize(i.readUnsignedShort(), i.readUnsignedShort())
        i.skipBytes(16)                              // server pixel format
        i.skipBytes(i.readInt())                     // name
        // 32 bpp, 0x00RRGGBB in little endian words
        o.writeByte(0); o.write(ByteArray(3))
        o.writeByte(32); o.writeByte(24); o.writeByte(0); o.writeByte(1)
        o.writeShort(255); o.writeShort(255); o.writeShort(255)
        o.writeByte(16); o.writeByte(8); o.writeByte(0); o.write(ByteArray(3))
        // encodings: raw, DesktopSize
        o.writeByte(2); o.writeByte(0); o.writeShort(2)
        o.writeInt(0); o.writeInt(-223)
        o.flush()
    }

    private fun setSize(w: Int, h: Int) {
        width = w; height = h
        bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        pixels = IntArray(w * h)
    }

    private fun readUpdate(i: DataInputStream) {
        i.readUnsignedByte()
        val rects = i.readUnsignedShort()
        var row = ByteArray(0)
        for (r in 0 until rects) {
            val x = i.readUnsignedShort(); val y = i.readUnsignedShort()
            val w = i.readUnsignedShort(); val h = i.readUnsignedShort()
            when (i.readInt()) {
                0 -> {
                    if (row.size < w * 4) row = ByteArray(w * 4)
                    for (yy in 0 until h) {
                        i.readFully(row, 0, w * 4)
                        var p = (y + yy) * width + x
                        var b = 0
                        for (xx in 0 until w) {
                            val v = (row[b].toInt() and 0xff) or
                                    ((row[b + 1].toInt() and 0xff) shl 8) or
                                    ((row[b + 2].toInt() and 0xff) shl 16)
                            if (p < pixels.size) pixels[p] = v or (0xff shl 24)
                            p++; b += 4
                        }
                    }
                }
                -223 -> setSize(w, h)
                else -> throw IllegalStateException("unexpected VNC encoding")
            }
        }
        bitmap?.setPixels(pixels, 0, width, 0, 0, width, height)
        onFrame()
        requestUpdate(true)
    }

    @Synchronized
    private fun requestUpdate(incremental: Boolean) {
        val o = out ?: return
        o.writeByte(3); o.writeByte(if (incremental) 1 else 0)
        o.writeShort(0); o.writeShort(0); o.writeShort(width); o.writeShort(height)
        o.flush()
    }

    @Synchronized
    fun pointer(x: Int, y: Int, down: Boolean) {
        val o = out ?: return
        try {
            o.writeByte(5); o.writeByte(if (down) 1 else 0)
            o.writeShort(x.coerceIn(0, width - 1)); o.writeShort(y.coerceIn(0, height - 1))
            o.flush()
        } catch (_: Exception) {}
    }

    @Synchronized
    fun key(keysym: Int, down: Boolean) {
        val o = out ?: return
        try {
            o.writeByte(4); o.writeByte(if (down) 1 else 0); o.writeShort(0)
            o.writeInt(keysym)
            o.flush()
        } catch (_: Exception) {}
    }

    companion object {
        /* X keysyms that the htc-leo machine maps to the HD2 buttons */
        const val KEY_VOLUP = 0xff55      // Page_Up
        const val KEY_VOLDOWN = 0xff56    // Page_Down
        const val KEY_WINDOWS = 0xffbe    // F1
        const val KEY_CALL = 0xffbf       // F2
        const val KEY_END = 0xffc0        // F3
        const val KEY_POWER = 0xffc1      // F4
        const val KEY_BACK = 0xff1b       // Escape
        const val KEY_HOME = 0xff50       // Home
    }
}
