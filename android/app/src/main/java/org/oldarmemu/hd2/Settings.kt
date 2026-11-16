package org.oldarmemu.hd2

import android.content.Context
import android.content.SharedPreferences
import kotlin.properties.ReadWriteProperty
import kotlin.reflect.KProperty

/** User settings of the app (the device's own state lives in [Storage]). */
class Settings(ctx: Context) {
    private val p: SharedPreferences =
        ctx.applicationContext.getSharedPreferences("settings", Context.MODE_PRIVATE)

    /* screen and controls */
    var scaleMode by str("scale", SCALE_FIT)
    var smooth by bool("smooth", true)
    var showFps by bool("fps", true)
    var haptics by bool("haptics", true)
    var sideButtons by bool("side_buttons", true)
    var bottomButtons by bool("bottom_buttons", true)
    var buttonSize by int("button_size", 48)            // dp
    var volumeKeys by bool("volume_keys", true)
    var immersive by bool("immersive", true)
    var keepScreenOn by bool("keep_on", true)

    /* performance */
    var tbSize by int("tb_size", 256)                   // MiB of translated code
    var pinCpu by bool("pin_cpu", true)

    /* storage */
    var sdSizeMb by int("sd_size", 1024)

    /* sound */
    var sound by bool("sound", true)
    var audioLatencyMs by int("audio_latency", 100)

    /* look */
    var theme by str("theme", THEME_SYSTEM)
    var dynamicColor by bool("dynamic_color", true)

    private fun bool(key: String, def: Boolean) = object : ReadWriteProperty<Any?, Boolean> {
        override fun getValue(thisRef: Any?, property: KProperty<*>) = p.getBoolean(key, def)
        override fun setValue(thisRef: Any?, property: KProperty<*>, value: Boolean) {
            p.edit().putBoolean(key, value).apply()
        }
    }

    private fun int(key: String, def: Int) = object : ReadWriteProperty<Any?, Int> {
        override fun getValue(thisRef: Any?, property: KProperty<*>) = p.getInt(key, def)
        override fun setValue(thisRef: Any?, property: KProperty<*>, value: Int) {
            p.edit().putInt(key, value).apply()
        }
    }

    private fun str(key: String, def: String) = object : ReadWriteProperty<Any?, String> {
        override fun getValue(thisRef: Any?, property: KProperty<*>) = p.getString(key, def) ?: def
        override fun setValue(thisRef: Any?, property: KProperty<*>, value: String) {
            p.edit().putString(key, value).apply()
        }
    }

    companion object {
        const val SCALE_FIT = "fit"
        const val SCALE_FILL = "fill"
        const val SCALE_INTEGER = "integer"
        const val THEME_SYSTEM = "system"
        const val THEME_LIGHT = "light"
        const val THEME_DARK = "dark"
        val TB_SIZES = listOf(128, 256, 512)
        val SD_SIZES = listOf(512, 1024, 2048, 4096)
        val LATENCIES = listOf(50, 100, 250)
    }
}
