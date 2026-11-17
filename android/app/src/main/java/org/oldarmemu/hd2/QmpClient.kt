package org.oldarmemu.hd2

import android.net.LocalSocket
import android.net.LocalSocketAddress
import org.json.JSONObject
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader

/**
 * Request/response QMP connection on QEMU's control socket (the input
 * socket of [QmpInput] is fire-and-forget).  Blocking: use off the UI thread.
 */
class QmpClient private constructor(private val s: LocalSocket) : AutoCloseable {
    private val rd = BufferedReader(InputStreamReader(s.inputStream))
    private val out = s.outputStream

    init {
        readReply()                                  // greeting
        cmd("qmp_capabilities")
    }

    /** Run [name]; returns its "return" object, throws on a QMP error. */
    fun cmd(name: String, args: JSONObject? = null): JSONObject {
        val req = JSONObject().put("execute", name)
        if (args != null) req.put("arguments", args)
        out.write((req.toString() + "\n").toByteArray())
        out.flush()
        val r = readReply()
        r.optJSONObject("error")?.let { throw IllegalStateException(it.optString("desc", "QMP error")) }
        return r.optJSONObject("return") ?: JSONObject()
    }

    fun status(): String = cmd("query-status").optString("status")

    private fun readReply(): JSONObject {
        while (true) {
            val line = rd.readLine() ?: throw IllegalStateException("QEMU closed the control socket")
            val o = JSONObject(line)
            if (!o.has("event")) return o                    // skip asynchronous events
        }
    }

    override fun close() {
        runCatching { s.close() }
    }

    companion object {
        /** Connect, waiting up to [timeoutMs] for QEMU to create the socket. */
        fun connect(sock: File, timeoutMs: Long = 20000): QmpClient {
            val deadline = System.currentTimeMillis() + timeoutMs
            while (true) {
                val s = LocalSocket()
                try {
                    s.connect(LocalSocketAddress(sock.absolutePath, LocalSocketAddress.Namespace.FILESYSTEM))
                    return QmpClient(s)
                } catch (e: Exception) {
                    runCatching { s.close() }
                    if (System.currentTimeMillis() > deadline) throw e
                    Thread.sleep(100)
                }
            }
        }
    }
}
