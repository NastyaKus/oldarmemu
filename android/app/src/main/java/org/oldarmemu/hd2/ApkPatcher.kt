package org.oldarmemu.hd2

import java.io.ByteArrayOutputStream
import java.io.File
import java.math.BigInteger
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.MessageDigest
import java.security.PrivateKey
import java.security.SecureRandom
import java.security.Signature
import java.security.spec.PKCS8EncodedKeySpec
import java.util.Base64
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

/**
 * Tweaks system APKs of a ROM so they run without a GPU.
 *
 * Gingerbread's Launcher2 draws its "all apps" screen with RenderScript
 * (OpenGL ES 2.0).  With the software renderer that SurfaceView stays
 * black and hides the whole home screen.  Launcher2 also ships a plain
 * 2D all-apps grid; we point its layout/all_apps alias to that one and
 * re-sign the APK (v1 JAR signature, which is all Android 2.x checks).
 */
object ApkPatcher {

    /** Patch Launcher2.apk; returns false if the APK has no 3D/2D switch. */
    fun launcher2d(apk: File, out: File, signer: JarSigner): Boolean {
        ZipFile(apk).use { zip ->
            val arsc = zip.getEntry("resources.arsc")?.let { e -> zip.getInputStream(e).use { it.readBytes() } }
                ?: return false
            val ids = Arsc.ids(arsc, "layout")
            val id3d = ids["all_apps_3d"] ?: return false
            val id2d = ids["all_apps_2d"] ?: return false
            val patched = HashMap<String, ByteArray>()
            for (e in zip.entries()) {
                if (!Regex("res/layout[^/]*/all_apps\\.xml").matches(e.name)) continue
                val xml = zip.getInputStream(e).use { it.readBytes() }
                if (replaceRef(xml, id3d, id2d) > 0) patched[e.name] = xml
            }
            if (patched.isEmpty()) return false
            signer.rewrite(zip, out, patched)
            return true
        }
    }

    /** Replace typed references (Res_value, type 0x01) to [from] in a binary XML. */
    internal fun replaceRef(xml: ByteArray, from: Int, to: Int): Int {
        val bb = ByteBuffer.wrap(xml).order(ByteOrder.LITTLE_ENDIAN)
        var n = 0
        var i = 0
        while (i + 8 <= xml.size) {
            if (bb.getShort(i).toInt() == 8 && xml[i + 2].toInt() == 0 && xml[i + 3].toInt() == 1 &&
                bb.getInt(i + 4) == from) {
                bb.putInt(i + 4, to)
                n++
            }
            i += 4
        }
        return n
    }
}

/** Minimal resources.arsc reader: resource name -> id for one type. */
object Arsc {
    private fun strings(b: ByteBuffer, at: Int): List<String> {
        val count = b.getInt(at + 8)
        val utf8 = b.getInt(at + 16) and 0x100 != 0
        val start = at + b.getInt(at + 20)
        return List(count) { k ->
            var p = start + b.getInt(at + b.getShort(at + 2) + 4 * k)
            if (utf8) {
                p += if (b.get(p).toInt() and 0x80 != 0) 2 else 1          // utf-16 length
                var len = b.get(p).toInt() and 0xff
                p++
                if (len and 0x80 != 0) { len = (len and 0x7f shl 8) or (b.get(p).toInt() and 0xff); p++ }
                String(ByteArray(len) { b.get(p + it) }, Charsets.UTF_8)
            } else {
                var len = b.getShort(p).toInt() and 0xffff
                p += 2
                if (len and 0x8000 != 0) { len = (len and 0x7fff shl 16) or (b.getShort(p).toInt() and 0xffff); p += 2 }
                String(CharArray(len) { b.getChar(p + 2 * it) })
            }
        }
    }

    fun ids(arsc: ByteArray, type: String): Map<String, Int> {
        val b = ByteBuffer.wrap(arsc).order(ByteOrder.LITTLE_ENDIAN)
        val out = HashMap<String, Int>()
        var p = b.getShort(2).toInt() and 0xffff                       // after table header
        while (p + 8 <= arsc.size) {
            val ctype = b.getShort(p).toInt() and 0xffff
            val csize = b.getInt(p + 4)
            if (ctype == 0x0200) {                                         // package
                val pkgId = b.getInt(p + 8)
                val typeNames = strings(b, p + b.getInt(p + 8 + 4 + 256))
                val keys = strings(b, p + b.getInt(p + 8 + 4 + 256 + 8))
                var q = p + (b.getShort(p + 2).toInt() and 0xffff)
                while (q < p + csize) {
                    val t = b.getShort(q).toInt() and 0xffff
                    val hs = b.getShort(q + 2).toInt() and 0xffff
                    val sz = b.getInt(q + 4)
                    if (t == 0x0201) {
                        val tid = b.get(q + 8).toInt() and 0xff
                        if (typeNames.getOrNull(tid - 1) == type) {
                            val count = b.getInt(q + 12)
                            val entries = q + b.getInt(q + 16)
                            for (k in 0 until count) {
                                val off = b.getInt(q + hs + 4 * k)
                                if (off == -1) continue
                                val key = keys.getOrNull(b.getInt(entries + off + 4)) ?: continue
                                out[key] = (pkgId shl 24) or (tid shl 16) or k
                            }
                        }
                    }
                    if (sz <= 0) break
                    q += sz
                }
            }
            if (csize <= 0) break
            p += csize
        }
        return out
    }
}

/**
 * APK v1 (JAR) signer with a self-signed key generated on first use and
 * kept in [dir]: Android 2.x verifies the signature but not who made it.
 */
class JarSigner(dir: File) {
    private val keyFile = File(dir, "compat-signer.pk8")
    private val certFile = File(dir, "compat-signer.crt")
    private val key: PrivateKey
    private val cert: ByteArray

    init {
        if (!keyFile.exists() || !certFile.exists()) {
            val kp = KeyPairGenerator.getInstance("RSA").apply { initialize(2048) }.generateKeyPair()
            val c = selfSigned(kp.public.encoded, kp.private)
            keyFile.writeBytes(kp.private.encoded)
            certFile.writeBytes(c)
        }
        key = KeyFactory.getInstance("RSA").generatePrivate(PKCS8EncodedKeySpec(keyFile.readBytes()))
        cert = certFile.readBytes()
    }

    /** Copy [zip] to [out] with [replace]d entries, dropping the old signature and signing anew. */
    fun rewrite(zip: ZipFile, out: File, replace: Map<String, ByteArray>) {
        val names = zip.entries().toList().filter { !it.isDirectory && !isSigFile(it.name) }
        fun data(e: ZipEntry) = replace[e.name] ?: zip.getInputStream(e).use { it.readBytes() }

        val mf = StringBuilder("Manifest-Version: 1.0\r\nCreated-By: 1.0 (hd2emu)\r\n\r\n")
        val sfEntries = StringBuilder()
        for (e in names) {
            val section = header("Name: ${e.name}") + "SHA1-Digest: ${b64(sha1(data(e)))}\r\n\r\n"
            mf.append(section)
            sfEntries.append(header("Name: ${e.name}") + "SHA1-Digest: ${b64(sha1(section.toByteArray()))}\r\n\r\n")
        }
        val mfBytes = mf.toString().toByteArray()
        val sf = ("Signature-Version: 1.0\r\nCreated-By: 1.0 (hd2emu)\r\n" +
            "SHA1-Digest-Manifest: ${b64(sha1(mfBytes))}\r\n\r\n" + sfEntries).toByteArray()
        val sig = Signature.getInstance("SHA1withRSA").run { initSign(key); update(sf); sign() }

        ZipOutputStream(out.outputStream().buffered(1 shl 16)).use { z ->
            fun put(name: String, bytes: ByteArray, stored: Boolean, time: Long) {
                val e = ZipEntry(name)
                e.time = time
                if (stored) {
                    e.method = ZipEntry.STORED
                    e.size = bytes.size.toLong()
                    e.compressedSize = bytes.size.toLong()
                    e.crc = CRC32().apply { update(bytes) }.value
                }
                z.putNextEntry(e); z.write(bytes); z.closeEntry()
            }
            val t = names.firstOrNull()?.time ?: 0L
            put("META-INF/MANIFEST.MF", mfBytes, false, t)
            put("META-INF/CERT.SF", sf, false, t)
            put("META-INF/CERT.RSA", pkcs7(sig), false, t)
            for (e in names) put(e.name, data(e), e.method == ZipEntry.STORED, e.time)
        }
    }

    private fun isSigFile(n: String): Boolean {
        if (!n.startsWith("META-INF/") || n.indexOf('/', 9) >= 0) return false
        val u = n.uppercase()
        return u == "META-INF/MANIFEST.MF" || u.endsWith(".SF") || u.endsWith(".RSA") ||
            u.endsWith(".DSA") || u.endsWith(".EC")
    }

    /** Manifest lines are at most 72 bytes; longer ones continue after a space. */
    private fun header(line: String): String {
        val b = line.toByteArray()
        if (b.size <= 70) return "$line\r\n"
        val sb = ByteArrayOutputStream()
        var i = 0
        var first = true
        while (i < b.size) {
            val n = minOf(if (first) 70 else 69, b.size - i)
            if (!first) sb.write(' '.code)
            sb.write(b, i, n); sb.write("\r\n".toByteArray())
            i += n; first = false
        }
        return sb.toString(Charsets.UTF_8.name())
    }

    private fun sha1(b: ByteArray) = MessageDigest.getInstance("SHA-1").digest(b)
    private fun b64(b: ByteArray) = Base64.getEncoder().encodeToString(b)

    private fun pkcs7(sig: ByteArray): ByteArray {
        val tbs = certTbs(cert)
        val issuer = tbsField(tbs, 3)
        val serial = tbsField(tbs, 1)
        val signerInfo = Der.seq(Der.int(1), Der.seq(issuer, serial), Der.SHA1, Der.RSA, Der.octets(sig))
        val signed = Der.seq(
            Der.int(1), Der.set(Der.SHA1), Der.seq(Der.OID_DATA),
            Der.tlv(0xA0, cert), Der.set(signerInfo))
        return Der.seq(Der.OID_SIGNED_DATA, Der.tlv(0xA0, signed))
    }

    companion object {
        private val NAME = Der.seq(Der.set(Der.seq(Der.OID_CN, Der.tlv(0x0C, "hd2emu compat".toByteArray()))))

        fun selfSigned(spki: ByteArray, key: PrivateKey): ByteArray {
            val serial = BigInteger(63, SecureRandom()).add(BigInteger.ONE)
            val tbs = Der.seq(
                Der.tlv(0x02, serial.toByteArray()), Der.SHA1_RSA, NAME,
                Der.seq(Der.tlv(0x17, "200101000000Z".toByteArray()), Der.tlv(0x17, "491231235959Z".toByteArray())),
                NAME, spki)
            val sig = Signature.getInstance("SHA1withRSA").run { initSign(key); update(tbs); sign() }
            return Der.seq(tbs, Der.SHA1_RSA, Der.tlv(0x03, byteArrayOf(0) + sig))
        }

        /* The TBSCertificate (first element of the certificate). */
        private fun certTbs(cert: ByteArray): ByteArray = Der.children(cert)[0]

        /* v1 TBS: serial(0) sigAlg(1) issuer(2) ...; v3 has an extra [0] version first. */
        private fun tbsField(tbs: ByteArray, v3Index: Int): ByteArray {
            val f = Der.children(tbs)
            val shift = if (f[0][0].toInt() and 0xff == 0xA0) 0 else 1
            return f[v3Index - shift]
        }
    }
}

/** Just enough DER for a certificate and a PKCS#7 SignedData. */
object Der {
    fun tlv(tag: Int, body: ByteArray): ByteArray {
        val o = ByteArrayOutputStream()
        o.write(tag)
        val n = body.size
        when {
            n < 0x80 -> o.write(n)
            n < 0x100 -> { o.write(0x81); o.write(n) }
            n < 0x10000 -> { o.write(0x82); o.write(n shr 8); o.write(n and 0xff) }
            else -> { o.write(0x83); o.write(n shr 16); o.write(n shr 8 and 0xff); o.write(n and 0xff) }
        }
        o.write(body)
        return o.toByteArray()
    }

    private fun cat(parts: Array<out ByteArray>) =
        ByteArrayOutputStream().apply { parts.forEach { write(it) } }.toByteArray()

    fun seq(vararg p: ByteArray) = tlv(0x30, cat(p))
    fun set(vararg p: ByteArray) = tlv(0x31, cat(p))
    fun int(v: Int) = tlv(0x02, BigInteger.valueOf(v.toLong()).toByteArray())
    fun octets(b: ByteArray) = tlv(0x04, b)
    private fun oid(vararg b: Int) = tlv(0x06, ByteArray(b.size) { b[it].toByte() })

    val OID_CN = oid(0x55, 0x04, 0x03)
    val OID_DATA = oid(0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x01)
    val OID_SIGNED_DATA = oid(0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02)
    private val NULL = byteArrayOf(0x05, 0x00)
    val SHA1 = seq(oid(0x2B, 0x0E, 0x03, 0x02, 0x1A), NULL)
    val RSA = seq(oid(0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x01), NULL)
    val SHA1_RSA = seq(oid(0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x05), NULL)

    /** The encoded elements inside a constructed value. */
    fun children(tlv: ByteArray): List<ByteArray> {
        fun len(at: Int): Pair<Int, Int> {               // (length, header size)
            val b = tlv[at + 1].toInt() and 0xff
            if (b < 0x80) return b to 2
            var n = 0
            for (k in 0 until (b and 0x7f)) n = (n shl 8) or (tlv[at + 2 + k].toInt() and 0xff)
            return n to 2 + (b and 0x7f)
        }
        val (total, hs) = len(0)
        val out = ArrayList<ByteArray>()
        var p = hs
        while (p < hs + total) {
            val (l, h) = len(p)
            out += tlv.copyOfRange(p, p + h + l)
            p += h + l
        }
        return out
    }
}
