package com.nertz.tabdisplay

import java.util.concurrent.atomic.AtomicLong

class Stats {
    private val rendered = AtomicLong()
    private val decodeUs = AtomicLong()
    private val bytes = AtomicLong()
    private val dropped = AtomicLong()

    private var lastNanos = System.nanoTime()
    private var lastRendered = 0L
    private var lastDecodeUs = 0L
    private var lastBytes = 0L

    var fps = 0.0
        private set
    var decodeMs = 0.0
        private set
    var mbps = 0.0
        private set
    val drops: Long get() = dropped.get()

    fun onBytes(count: Long) {
        bytes.addAndGet(count)
    }

    fun onRendered(us: Long) {
        decodeUs.addAndGet(us)
        rendered.incrementAndGet()
    }

    fun onDropped() {
        dropped.incrementAndGet()
    }

    fun resetDrops() {
        dropped.set(0)
    }

    fun tick() {
        val now = System.nanoTime()
        val seconds = (now - lastNanos) / 1e9
        if (seconds <= 0.0) return
        val r = rendered.get()
        val d = decodeUs.get()
        val b = bytes.get()
        val frames = r - lastRendered
        fps = frames / seconds
        decodeMs = if (frames > 0) (d - lastDecodeUs) / 1000.0 / frames else 0.0
        mbps = (b - lastBytes) * 8.0 / seconds / 1e6
        lastNanos = now
        lastRendered = r
        lastDecodeUs = d
        lastBytes = b
    }
}
