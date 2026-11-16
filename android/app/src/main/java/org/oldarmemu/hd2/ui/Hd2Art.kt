package org.oldarmemu.hd2.ui

import android.graphics.Bitmap
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp

/**
 * The HTC HD2 drawn in code: graphite body, chrome rim, 4.3" screen
 * (showing the emulator's last frame when there is one) and the five
 * buttons under it.
 */
@Composable
fun Hd2Art(frame: Bitmap?, modifier: Modifier = Modifier, width: Dp = 128.dp) {
    val img = remember(frame) { frame?.asImageBitmap() }
    Canvas(modifier.size(width, width * 1.85f)) {
        val w = size.width
        val h = size.height
        val r = w * 0.09f
        // body
        drawRoundRect(
            Brush.verticalGradient(listOf(Color(0xFF3A3D40), Color(0xFF16181A))),
            cornerRadius = CornerRadius(r),
        )
        // chrome rim
        drawRoundRect(
            Brush.linearGradient(listOf(Color(0xFFDADDE0), Color(0xFF7D8186), Color(0xFFE8EAEC)),
                start = Offset.Zero, end = Offset(w, h)),
            cornerRadius = CornerRadius(r), style = Stroke(w * 0.025f),
        )
        // screen
        val sx = w * 0.07f
        val sy = h * 0.075f
        val sw = w - 2 * sx
        val sh = sw * 800f / 480f
        if (img != null) {
            drawImage(img, srcOffset = IntOffset.Zero, srcSize = IntSize(img.width, img.height),
                dstOffset = IntOffset(sx.toInt(), sy.toInt()), dstSize = IntSize(sw.toInt(), sh.toInt()))
        } else {
            drawRect(
                Brush.linearGradient(listOf(Color(0xFF0B3D2E), Color(0xFF0A1F33), Color(0xFF3DDC84)),
                    start = Offset(sx, sy), end = Offset(sx + sw, sy + sh)),
                topLeft = Offset(sx, sy), size = Size(sw, sh),
            )
        }
        drawRect(Color(0x33FFFFFF), topLeft = Offset(sx, sy), size = Size(sw, sh),
            style = Stroke(1.5f))
        // glass reflection
        drawRect(
            Brush.linearGradient(listOf(Color(0x22FFFFFF), Color.Transparent),
                start = Offset(sx, sy), end = Offset(sx + sw * 0.6f, sy + sh * 0.5f)),
            topLeft = Offset(sx, sy), size = Size(sw, sh),
        )
        // earpiece
        drawRoundRect(Color(0xFF55595D), topLeft = Offset(w * 0.38f, h * 0.03f),
            size = Size(w * 0.24f, h * 0.012f), cornerRadius = CornerRadius(h * 0.006f))
        // five buttons: call, home, windows, back, end
        val by = sy + sh + (h - sy - sh) * 0.32f
        val bh = (h - sy - sh) * 0.38f
        val gap = w * 0.02f
        val bw = (w - 2 * sx - 4 * gap) / 5
        val tint = listOf(Color(0xFF3DDC84), Color(0xFFBFC3C7), Color(0xFFBFC3C7),
            Color(0xFFBFC3C7), Color(0xFFE5534B))
        for (i in 0 until 5) {
            val bx = sx + i * (bw + gap)
            drawRoundRect(Color(0xFF26292C), topLeft = Offset(bx, by), size = Size(bw, bh),
                cornerRadius = CornerRadius(bh * 0.3f))
            drawCircle(tint[i], radius = bh * 0.16f, center = Offset(bx + bw / 2, by + bh / 2))
        }
    }
}
