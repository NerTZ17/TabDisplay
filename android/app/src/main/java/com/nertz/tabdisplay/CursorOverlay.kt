package com.nertz.tabdisplay

import android.graphics.Bitmap
import android.os.Handler
import android.view.View
import android.widget.ImageView
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import kotlin.math.roundToInt

class CursorOverlay(private val view: ImageView, private val handler: Handler) {
    private var bitmap: Bitmap? = null
    private var hotX = 0
    private var hotY = 0
    private var x = 0
    private var y = 0
    private var visible = false
    private var left = 0
    private var top = 0
    private var scale = 1f

    private val latest = AtomicLong()
    private val pending = AtomicBoolean()
    private val applyPos = Runnable {
        pending.set(false)
        val v = latest.get()
        x = (v shr 32).toInt().toShort().toInt()
        y = (v shr 16).toInt().toShort().toInt()
        visible = v and 1L != 0L
        place()
    }

    init {
        view.scaleType = ImageView.ScaleType.FIT_XY
        view.visibility = View.INVISIBLE
    }

    fun postShape(b: Bitmap, hx: Int, hy: Int) {
        handler.post { setShape(b, hx, hy) }
    }

    fun postPos(px: Int, py: Int, show: Boolean) {
        latest.set(((px.toLong() and 0xffff) shl 32) or ((py.toLong() and 0xffff) shl 16) or (if (show) 1L else 0L))
        if (pending.compareAndSet(false, true)) handler.post(applyPos)
    }

    fun setVideoRect(l: Int, t: Int, width: Int, streamWidth: Int) {
        left = l
        top = t
        scale = if (streamWidth > 0 && width > 0) width.toFloat() / streamWidth else 1f
        resize()
        place()
    }

    fun clear() {
        handler.removeCallbacks(applyPos)
        pending.set(false)
        bitmap = null
        visible = false
        view.setImageDrawable(null)
        view.visibility = View.INVISIBLE
    }

    private fun setShape(b: Bitmap, hx: Int, hy: Int) {
        bitmap = b
        hotX = hx
        hotY = hy
        view.setImageBitmap(b)
        resize()
        place()
    }

    private fun resize() {
        val b = bitmap ?: return
        val w = (b.width * scale).roundToInt().coerceAtLeast(1)
        val h = (b.height * scale).roundToInt().coerceAtLeast(1)
        val lp = view.layoutParams
        if (lp.width != w || lp.height != h) {
            lp.width = w
            lp.height = h
            view.layoutParams = lp
        }
    }

    private fun place() {
        if (bitmap == null || !visible) {
            if (view.visibility != View.INVISIBLE) view.visibility = View.INVISIBLE
            return
        }
        view.translationX = left + (x - hotX) * scale
        view.translationY = top + (y - hotY) * scale
        if (view.visibility != View.VISIBLE) view.visibility = View.VISIBLE
    }
}
