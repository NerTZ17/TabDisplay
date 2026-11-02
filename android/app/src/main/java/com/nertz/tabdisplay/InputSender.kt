package com.nertz.tabdisplay

import android.os.Handler
import android.os.SystemClock
import android.view.InputDevice
import android.view.MotionEvent
import android.view.View
import kotlin.math.PI
import kotlin.math.atan
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.tan

class InputSender(private val view: View, private val handler: Handler) : View.OnTouchListener, View.OnHoverListener {
    @Volatile var sender: Sender? = null

    private var session: Sender? = null
    private val ids = IntArray(MAX_FINGERS)
    private val flags = IntArray(MAX_FINGERS)
    private val xs = IntArray(MAX_FINGERS)
    private val ys = IntArray(MAX_FINGERS)
    private val lastX = IntArray(MAX_FINGERS)
    private val lastY = IntArray(MAX_FINGERS)
    private var activeFingers = 0
    private var rejectedFingers = 0

    private var penInRange = false
    private var penContact = false
    private var penEraser = false
    private var penLastMs = 0L
    private var penX = 0
    private var penY = 0
    private val penLeave = Runnable { leaveRange() }

    init {
        view.setOnTouchListener(this)
        view.setOnHoverListener(this)
        view.post { view.requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_POINTER) }
    }

    @Suppress("ClickableViewAccessibility")
    override fun onTouch(v: View, event: MotionEvent): Boolean {
        val s = current() ?: return true
        val p = stylusIndex(event)
        if (p >= 0) penTouch(s, event, p) else fingers(s, event)
        return true
    }

    override fun onHover(v: View, event: MotionEvent): Boolean {
        val s = current() ?: return false
        val p = stylusIndex(event)
        if (p < 0) return false
        penEraser = event.getToolType(p) == MotionEvent.TOOL_TYPE_ERASER
        when (event.actionMasked) {
            MotionEvent.ACTION_HOVER_ENTER, MotionEvent.ACTION_HOVER_MOVE -> {
                handler.removeCallbacks(penLeave)
                if (activeFingers != 0) cancelFingers(s)
                penInRange = true
                penContact = false
                penSamples(s, event, p, Protocol.PEN_IN_RANGE)
            }
            MotionEvent.ACTION_HOVER_EXIT -> {
                handler.removeCallbacks(penLeave)
                handler.postDelayed(penLeave, LEAVE_DELAY_MS)
            }
        }
        penLastMs = event.eventTime
        return true
    }

    private fun current(): Sender? {
        val s = sender
        if (s !== session) {
            session = s
            handler.removeCallbacks(penLeave)
            activeFingers = 0
            rejectedFingers = 0
            penInRange = false
            penContact = false
        }
        return s
    }

    private fun stylusIndex(e: MotionEvent): Int {
        for (i in 0 until e.pointerCount) {
            val tool = e.getToolType(i)
            if (tool == MotionEvent.TOOL_TYPE_STYLUS || tool == MotionEvent.TOOL_TYPE_ERASER) return i
        }
        return -1
    }

    private fun penTouch(s: Sender, e: MotionEvent, p: Int) {
        handler.removeCallbacks(penLeave)
        if (activeFingers != 0) cancelFingers(s)
        penEraser = e.getToolType(p) == MotionEvent.TOOL_TYPE_ERASER
        val action = e.actionMasked
        when {
            action == MotionEvent.ACTION_CANCEL -> {
                penContact = false
                penInRange = false
                s.pen(eraserBit(), penX, penY, 0, 0, 0, 0)
            }
            action == MotionEvent.ACTION_UP || (action == MotionEvent.ACTION_POINTER_UP && e.actionIndex == p) -> {
                penContact = false
                penInRange = true
                penSample(s, e, p, -1, Protocol.PEN_IN_RANGE)
                handler.postDelayed(penLeave, LEAVE_DELAY_MS)
            }
            else -> {
                penContact = true
                penInRange = true
                penSamples(s, e, p, Protocol.PEN_IN_RANGE or Protocol.PEN_CONTACT)
            }
        }
        penLastMs = e.eventTime
    }

    private fun penSamples(s: Sender, e: MotionEvent, p: Int, base: Int) {
        for (h in 0 until e.historySize) penSample(s, e, p, h, base)
        penSample(s, e, p, -1, base)
    }

    private fun penSample(s: Sender, e: MotionEvent, p: Int, h: Int, base: Int) {
        val x: Float
        val y: Float
        val pressure: Float
        val tilt: Float
        val orientation: Float
        if (h < 0) {
            x = e.getX(p)
            y = e.getY(p)
            pressure = e.getPressure(p)
            tilt = e.getAxisValue(MotionEvent.AXIS_TILT, p)
            orientation = e.getAxisValue(MotionEvent.AXIS_ORIENTATION, p)
        } else {
            x = e.getHistoricalX(p, h)
            y = e.getHistoricalY(p, h)
            pressure = e.getHistoricalPressure(p, h)
            tilt = e.getHistoricalAxisValue(MotionEvent.AXIS_TILT, p, h)
            orientation = e.getHistoricalAxisValue(MotionEvent.AXIS_ORIENTATION, p, h)
        }
        val barrel = if (e.buttonState and (MotionEvent.BUTTON_STYLUS_PRIMARY or MotionEvent.BUTTON_SECONDARY) != 0) Protocol.PEN_BARREL else 0
        val f = base or barrel or eraserBit()
        penX = normX(x)
        penY = normY(y)
        val level = if (f and Protocol.PEN_CONTACT != 0) (pressure.coerceIn(0f, 1f) * 1024f).roundToInt() else 0
        val o = orientation.toDouble()
        val t = tan(tilt.toDouble().coerceIn(0.0, MAX_TILT))
        val tiltX = Math.toDegrees(atan(t * sin(o))).roundToInt().coerceIn(-90, 90)
        val tiltY = Math.toDegrees(atan(t * -cos(o))).roundToInt().coerceIn(-90, 90)
        val rotation = ((Math.toDegrees(o).roundToInt() % 360) + 360) % 360
        s.pen(f, penX, penY, level, tiltX, tiltY, rotation)
    }

    private fun leaveRange() {
        val s = sender
        if (penInRange && s != null && s === session) s.pen(eraserBit(), penX, penY, 0, 0, 0, 0)
        penInRange = false
        penContact = false
        penLastMs = SystemClock.uptimeMillis()
    }

    private fun eraserBit() = if (penEraser) Protocol.PEN_ERASER else 0

    private fun penBlocking(time: Long) = penInRange || penContact || time - penLastMs < PALM_WINDOW_MS

    private fun fingers(s: Sender, e: MotionEvent) {
        val action = e.actionMasked
        if (action == MotionEvent.ACTION_DOWN) rejectedFingers = 0
        val ending = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL
        if (penBlocking(e.eventTime)) {
            for (i in 0 until e.pointerCount) {
                val id = e.getPointerId(i)
                if (id < MAX_FINGERS) rejectedFingers = rejectedFingers or (1 shl id)
            }
            if (activeFingers != 0) cancelFingers(s)
            if (ending) rejectedFingers = 0
            return
        }
        val lift = if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) e.actionIndex else -1
        val palm = action == MotionEvent.ACTION_POINTER_UP && e.flags and FLAG_CANCELED != 0
        var n = 0
        for (i in 0 until e.pointerCount) {
            val id = e.getPointerId(i)
            if (id >= MAX_FINGERS) continue
            val bit = 1 shl id
            if (rejectedFingers and bit != 0) {
                if (i == lift) rejectedFingers = rejectedFingers and bit.inv()
                continue
            }
            val f = when {
                action == MotionEvent.ACTION_CANCEL -> Protocol.TOUCH_CANCELED
                i == lift -> if (palm) Protocol.TOUCH_CANCELED else 0
                else -> Protocol.TOUCH_CONTACT
            }
            val x = normX(e.getX(i))
            val y = normY(e.getY(i))
            ids[n] = id
            flags[n] = f
            xs[n] = x
            ys[n] = y
            lastX[id] = x
            lastY[id] = y
            activeFingers = if (f and Protocol.TOUCH_CONTACT != 0) activeFingers or bit else activeFingers and bit.inv()
            n++
        }
        if (n > 0) s.touch(n, ids, flags, xs, ys)
        if (ending) {
            activeFingers = 0
            rejectedFingers = 0
        }
    }

    private fun cancelFingers(s: Sender) {
        var n = 0
        for (id in 0 until MAX_FINGERS) {
            if (activeFingers and (1 shl id) == 0) continue
            ids[n] = id
            flags[n] = Protocol.TOUCH_CANCELED
            xs[n] = lastX[id]
            ys[n] = lastY[id]
            n++
        }
        if (n > 0) s.touch(n, ids, flags, xs, ys)
        rejectedFingers = rejectedFingers or activeFingers
        activeFingers = 0
    }

    private fun normX(x: Float): Int {
        val w = view.width
        return if (w <= 0) 0 else ((x / w).coerceIn(0f, 1f) * 65535f).roundToInt()
    }

    private fun normY(y: Float): Int {
        val h = view.height
        return if (h <= 0) 0 else ((y / h).coerceIn(0f, 1f) * 65535f).roundToInt()
    }

    private companion object {
        const val MAX_FINGERS = 10
        const val PALM_WINDOW_MS = 300L
        const val LEAVE_DELAY_MS = 100L
        const val FLAG_CANCELED = 0x20
        const val MAX_TILT = PI / 2
    }
}
