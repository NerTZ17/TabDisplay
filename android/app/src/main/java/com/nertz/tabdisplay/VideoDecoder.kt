package com.nertz.tabdisplay

import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Process
import android.util.Log
import android.view.Surface
import java.nio.ByteBuffer
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

class VideoDecoder(private val stats: Stats) {
    @Volatile var sender: Sender? = null
    @Volatile var choices: List<DecoderChoice> = emptyList()

    private val lock = ReentrantLock()
    private val surfaceReady = lock.newCondition()
    private val thread = HandlerThread("tabdisplay-decode").apply { start() }
    private val handler = Handler(thread.looper)
    private val inputs = ArrayBlockingQueue<Int>(256)

    @Volatile private var codec: MediaCodec? = null
    @Volatile private var resetRequested = false
    @Volatile private var closed = false
    private var surface: Surface? = null
    private var config: StreamConfig? = null
    private var waiting = false
    private var needIdr = true
    private var lastRequestNanos = 0L
    private var csd = ByteArray(0)
    private var csdLength = 0
    private var csdPending = false
    private var inputBuffer: ByteBuffer? = null

    private var baseFrameId = -1L
    private var ptsOffset = 0L
    private var lastPts = -1L

    private val ringLock = Any()
    private val ringPts = LongArray(RING) { -1L }
    private val ringFrame = LongArray(RING)
    private val ringHost = LongArray(RING)
    private val ringReceive = LongArray(RING)
    private var ringNext = 0

    private val callback = object : MediaCodec.Callback() {
        override fun onInputBufferAvailable(c: MediaCodec, index: Int) {
            if (c === codec) inputs.offer(index)
        }

        override fun onOutputBufferAvailable(c: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
            if (c !== codec) return
            try {
                c.releaseOutputBuffer(index, System.nanoTime())
            } catch (e: IllegalStateException) {
                return
            }
            if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) return
            acknowledge(info.presentationTimeUs, System.nanoTime())
        }

        override fun onError(c: MediaCodec, e: MediaCodec.CodecException) {
            if (c !== codec) return
            Log.w(TAG, "codec error transient=${e.isTransient} recoverable=${e.isRecoverable}", e)
            if (!e.isTransient) resetRequested = true
            sender?.requestKeyframe()
        }

        override fun onOutputFormatChanged(c: MediaCodec, format: MediaFormat) {
            Log.i(TAG, "output format $format")
        }
    }

    init {
        handler.post { runCatching { Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY) } }
    }

    fun start(config: StreamConfig) = lock.withLock {
        releaseLocked()
        this.config = config
        csdLength = 0
        csdPending = false
        val deadline = System.nanoTime() + SURFACE_WAIT_NANOS
        waiting = true
        while (surface == null && !closed && System.nanoTime() < deadline) {
            surfaceReady.await(100, TimeUnit.MILLISECONDS)
        }
        waiting = false
        if (surface != null) createLocked()
    }

    fun stop() = lock.withLock {
        releaseLocked()
        config = null
        csdLength = 0
        csdPending = false
    }

    fun attachSurface(s: Surface) = lock.withLock {
        surface = s
        if (waiting) {
            surfaceReady.signalAll()
        } else if (config != null && codec == null) {
            createLocked()
            requestKeyframeLocked()
        }
    }

    fun detachSurface() = lock.withLock {
        releaseLocked()
        surface = null
    }

    fun shutdown() {
        closed = true
        lock.withLock { surfaceReady.signalAll() }
        stop()
        thread.quitSafely()
    }

    fun feed(data: ByteArray, offset: Int, length: Int, frameId: Long, hostTimeUs: Long, flags: Int, receiveNanos: Long) {
        lock.withLock { feedLocked(data, offset, length, frameId, hostTimeUs, flags, receiveNanos) }
    }

    private fun feedLocked(data: ByteArray, offset: Int, length: Int, frameId: Long, hostTimeUs: Long, flags: Int, receiveNanos: Long) {
        if (resetRequested && config != null && surface != null) {
            releaseLocked()
            createLocked()
            requestKeyframeLocked()
        }
        val isConfig = flags and Protocol.FRAME_CODEC_CONFIG != 0
        val isIdr = flags and Protocol.FRAME_IDR != 0
        if (isConfig) {
            storeCsd(data, offset, length)
            csdPending = true
        }
        val c = codec
        if (c == null) {
            if (!isConfig) dropLocked()
            return
        }
        try {
            if (csdPending && !queueCsdLocked(c)) {
                dropLocked()
                return
            }
            if (isConfig) return
            if (needIdr && !isIdr) {
                stats.onDropped()
                if (System.nanoTime() - lastRequestNanos > REREQUEST_NANOS) requestKeyframeLocked()
                return
            }
            val index = takeInputLocked(c)
            if (index < 0) {
                dropLocked()
                return
            }
            val buffer = inputBuffer!!
            if (length > buffer.capacity()) {
                inputs.offer(index)
                dropLocked()
                return
            }
            buffer.clear()
            buffer.put(data, offset, length)
            val pts = ptsFor(frameId)
            record(pts, frameId, hostTimeUs, receiveNanos)
            c.queueInputBuffer(index, 0, length, pts, if (isIdr) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0)
            if (isIdr) needIdr = false
        } catch (e: MediaCodec.CodecException) {
            Log.w(TAG, "queue failed", e)
            if (!e.isTransient) resetRequested = true
            dropLocked()
        } catch (e: IllegalStateException) {
            Log.w(TAG, "queue failed", e)
            resetRequested = true
            dropLocked()
        }
    }

    private fun takeInputLocked(c: MediaCodec): Int {
        val deadline = System.nanoTime() + INPUT_WAIT_NANOS
        while (true) {
            val left = deadline - System.nanoTime()
            val index = (if (left > 0) inputs.poll(left, TimeUnit.NANOSECONDS) else inputs.poll()) ?: return -1
            val buffer = c.getInputBuffer(index) ?: continue
            inputBuffer = buffer
            return index
        }
    }

    private fun queueCsdLocked(c: MediaCodec): Boolean {
        if (csdLength == 0) {
            csdPending = false
            return true
        }
        val index = takeInputLocked(c)
        if (index < 0) return false
        val buffer = inputBuffer!!
        if (csdLength > buffer.capacity()) {
            inputs.offer(index)
            return false
        }
        buffer.clear()
        buffer.put(csd, 0, csdLength)
        c.queueInputBuffer(index, 0, csdLength, maxOf(lastPts, 0L), MediaCodec.BUFFER_FLAG_CODEC_CONFIG)
        csdPending = false
        return true
    }

    private fun storeCsd(data: ByteArray, offset: Int, length: Int) {
        if (csd.size < length) csd = ByteArray(length)
        System.arraycopy(data, offset, csd, 0, length)
        csdLength = length
    }

    private fun dropLocked() {
        stats.onDropped()
        if (!needIdr) {
            needIdr = true
            requestKeyframeLocked()
        } else if (System.nanoTime() - lastRequestNanos > REREQUEST_NANOS) {
            requestKeyframeLocked()
        }
    }

    private fun requestKeyframeLocked() {
        lastRequestNanos = System.nanoTime()
        sender?.requestKeyframe()
    }

    private fun ptsFor(frameId: Long): Long {
        if (baseFrameId < 0) baseFrameId = frameId
        var pts = (frameId - baseFrameId) * FRAME_SPACING_US + ptsOffset
        if (pts <= lastPts) {
            ptsOffset += lastPts + FRAME_SPACING_US - pts
            pts = lastPts + FRAME_SPACING_US
        }
        lastPts = pts
        return pts
    }

    private fun record(pts: Long, frameId: Long, hostTimeUs: Long, receiveNanos: Long) = synchronized(ringLock) {
        val slot = ringNext
        ringNext = (slot + 1) and (RING - 1)
        ringPts[slot] = pts
        ringFrame[slot] = frameId
        ringHost[slot] = hostTimeUs
        ringReceive[slot] = receiveNanos
    }

    private fun acknowledge(pts: Long, nowNanos: Long) {
        var frameId = 0L
        var hostTimeUs = 0L
        var receiveNanos = 0L
        val found = synchronized(ringLock) {
            var slot = (ringNext - 1) and (RING - 1)
            var hit = false
            for (i in 0 until RING) {
                if (ringPts[slot] == pts) {
                    frameId = ringFrame[slot]
                    hostTimeUs = ringHost[slot]
                    receiveNanos = ringReceive[slot]
                    ringPts[slot] = -1L
                    hit = true
                    break
                }
                slot = (slot - 1) and (RING - 1)
            }
            hit
        }
        if (!found) return
        val decodeUs = (nowNanos - receiveNanos) / 1000L
        sender?.frameAck(frameId, hostTimeUs, decodeUs)
        stats.onRendered(decodeUs)
    }

    private fun createLocked() {
        val cfg = config ?: return
        val s = surface ?: return
        val mime = Protocol.mime(cfg.codec) ?: return
        val choice = choices.firstOrNull { it.codec == cfg.codec }
        inputs.clear()
        synchronized(ringLock) { ringPts.fill(-1L) }
        baseFrameId = -1L
        ptsOffset = 0L
        lastPts = -1L
        needIdr = true
        lastRequestNanos = System.nanoTime()
        resetRequested = false
        csdPending = csdLength > 0
        codec = try {
            configure(choice, mime, cfg, s, extended = true)
        } catch (e: Exception) {
            Log.w(TAG, "configure with extended keys failed", e)
            try {
                configure(choice, mime, cfg, s, extended = false)
            } catch (e2: Exception) {
                Log.e(TAG, "configure failed", e2)
                null
            }
        }
    }

    private fun configure(choice: DecoderChoice?, mime: String, cfg: StreamConfig, s: Surface, extended: Boolean): MediaCodec {
        val c = if (choice != null) MediaCodec.createByCodecName(choice.name) else MediaCodec.createDecoderByType(mime)
        try {
            val format = MediaFormat.createVideoFormat(mime, cfg.width, cfg.height)
            format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, cfg.width * cfg.height)
            format.setInteger(MediaFormat.KEY_PRIORITY, 0)
            if (Build.VERSION.SDK_INT >= 30 && choice?.lowLatency == true) format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            if (extended) {
                val max = choice?.maxFps ?: 0
                val rate = if (max > 0) cfg.fps.coerceIn(1, max) else cfg.fps
                if (rate > 0) format.setInteger(MediaFormat.KEY_OPERATING_RATE, rate)
                if (choice?.name?.contains("mtk", ignoreCase = true) == true) format.setInteger(MTK_LOW_LATENCY, 1)
            }
            codec = c
            c.setCallback(callback, handler)
            c.configure(format, s, null, 0)
            c.start()
            Log.i(TAG, "decoder ${c.name} started $format")
            return c
        } catch (e: Exception) {
            codec = null
            runCatching { c.release() }
            throw e
        }
    }

    private fun releaseLocked() {
        val c = codec ?: return
        codec = null
        runCatching { c.stop() }
        runCatching { c.release() }
        inputs.clear()
        inputBuffer = null
    }

    private companion object {
        const val TAG = "TabDisplay"
        const val RING = 64
        const val FRAME_SPACING_US = 1000L
        const val INPUT_WAIT_NANOS = 20_000_000L
        const val REREQUEST_NANOS = 500_000_000L
        const val SURFACE_WAIT_NANOS = 3_000_000_000L
        const val MTK_LOW_LATENCY = "vdec-lowlatency"
    }
}
