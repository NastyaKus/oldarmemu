package org.oldarmemu.hd2.ui

import app.cash.paparazzi.DeviceConfig
import app.cash.paparazzi.Paparazzi
import com.android.resources.NightMode
import org.junit.Rule
import org.junit.Test

/** Renders the main screens on the JVM (./gradlew recordPaparazziDebug). */
class ScreensSnapshotTest {
    @get:Rule
    val paparazzi = Paparazzi(deviceConfig = DeviceConfig.PIXEL_6.copy(nightMode = NightMode.NIGHT),
        maxPercentDifference = 1.0)

    private val prefs = MemPrefs()
    private val tmp = java.nio.file.Files.createTempDirectory("hd2ui").toFile()

    /* layoutlib's context has no data directory: give it one, with demo files */
    private fun state(): AppState {
        val ctx = object : android.content.ContextWrapper(paparazzi.context) {
            override fun getFilesDir() = java.io.File(tmp, "files").apply { mkdirs() }
            override fun getCacheDir() = java.io.File(tmp, "cache").apply { mkdirs() }
            override fun getApplicationContext() = this
            override fun getSharedPreferences(n: String?, mode: Int) = prefs
        }
        val s = AppState(ctx)
        s.cfg.dynamicColor = false
        java.io.File(s.st.dir, "recovery.img").writeBytes(ByteArray(4096))
        java.io.File(s.st.sdcard, "NexusHD2-JellyBean-CM10.2_V3.1.zip").writeBytes(ByteArray(300000))
        java.io.File(s.st.sdcard, "gapps-jb-20130813.zip").writeBytes(ByteArray(90000))
        java.io.File(s.st.sdcard, "zz-hd2emu-compat.zip").writeBytes(ByteArray(2000))
        return s
    }

    @Test fun home() {
        val s = state()
        paparazzi.snapshot { Hd2Theme(s.cfg) { androidx.compose.material3.Surface { HomeScreen(s, {}, {}) } } }
    }

    @Test fun sd() {
        val s = state()
        paparazzi.snapshot { Hd2Theme(s.cfg) { androidx.compose.material3.Surface { SdScreen(s) {} } } }
    }

    @Test fun settingsLight() {
        val s = state()
        s.cfg.theme = org.oldarmemu.hd2.Settings.THEME_LIGHT
        paparazzi.snapshot { Hd2Theme(s.cfg) { androidx.compose.material3.Surface { HomeScreen(s, {}, {}) } } }
    }

    @Test fun settings() {
        val s = state()
        paparazzi.snapshot { Hd2Theme(s.cfg) { androidx.compose.material3.Surface { SettingsScreen(s, {}, {}, {}) {} } } }
    }

    @Test fun app() {
        val s = state()
        paparazzi.snapshot {
            val owner = object : androidx.lifecycle.ViewModelStoreOwner {
                override val viewModelStore = androidx.lifecycle.ViewModelStore()
            }
            androidx.compose.runtime.CompositionLocalProvider(
                androidx.lifecycle.viewmodel.compose.LocalViewModelStoreOwner provides owner) {
                Hd2Theme(s.cfg) { App(s, Actions({}, {}, {}, {}, {})) }
            }
        }
    }

    @Test fun nandBackupRoundTrip() {
        val s = state()
        val st = s.st
        st.setNandLayout(org.oldarmemu.hd2.Storage.LAYOUTS[3])
        val data = ByteArray(3 shl 20) { (it * 31 + it / 4096).toByte() }
        st.nand.writeBytes(data)
        val out = java.io.ByteArrayOutputStream()
        var last = 0f
        st.backupNand(out) { last = it }
        org.junit.Assert.assertEquals(1f, last, 0.001f)
        st.setNandLayout(org.oldarmemu.hd2.Storage.LAYOUTS[0])      // erases
        org.junit.Assert.assertFalse(st.nand.exists())
        st.restoreNand(java.io.ByteArrayInputStream(out.toByteArray())) {}
        org.junit.Assert.assertArrayEquals(data, st.nand.readBytes())
        org.junit.Assert.assertEquals(org.oldarmemu.hd2.Storage.LAYOUTS[3], st.nandLayout())
        try {
            st.restoreNand(java.io.ByteArrayInputStream(ByteArray(10))) {}
            org.junit.Assert.fail("garbage accepted")
        } catch (e: IllegalArgumentException) { }
        org.junit.Assert.assertArrayEquals(data, st.nand.readBytes())
    }
}
