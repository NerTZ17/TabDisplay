package com.nertz.tabdisplay

import android.os.Process
import java.io.BufferedOutputStream
import java.io.IOException
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

class Sender(private val socket: Socket) {
    private val out = BufferedOutputStream(socket.getOutputStream(), 64 * 1024)
    private val lock = ReentrantLock()
    private val ready = lock.newCondition()
    private var front = ByteBuffer.allocate(CAPACITY).order(ByteOrder.LITTLE_ENDIAN)
    private var back = ByteBuffer.allocate(CAPACITY).order(ByteOrder.LITTLE_ENDIAN)
    private var closed = false
    private val thread = Thread(::drain, "tabdisplay-send")

    init {
        thread.start()
    }

    fun clientHello(panel: PanelInfo, codecs: List<DecoderChoice>) = put(Protocol.CLIENT_HELLO, 13 + codecs.size * 8) { b ->
        b.put(Protocol.MAGIC)
        b.putShort(Protocol.VERSION.toShort())
        b.putShort(panel.width.toShort())
        b.putShort(panel.height.toShort())
        b.putShort(panel.maxRefreshHz.toShort())
        b.put(codecs.size.toByte())
        for (c in codecs) {
            b.put(c.codec.toByte())
            b.put(c.flags.toByte())
            b.putShort(c.maxFps.toShort())
            b.putShort(c.maxWidth.toShort())
            b.putShort(c.maxHeight.toShort())
        }
    }

    fun frameAck(frameId: Long, hostTimeUs: Long, decodeUs: Long) = put(Protocol.FRAME_ACK, 16) { b ->
        b.putInt(frameId.toInt())
        b.putLong(hostTimeUs)
        b.putInt(decodeUs.coerceIn(0L, 0xffffffffL).toInt())
    }

    fun touch(count: Int, ids: IntArray, flags: IntArray, xs: IntArray, ys: IntArray) = put(Protocol.TOUCH, 1 + count * 6) { b ->
        b.put(count.toByte())
        for (i in 0 until count) {
            b.put(ids[i].toByte())
            b.put(flags[i].toByte())
            b.putShort(xs[i].toShort())
            b.putShort(ys[i].toShort())
        }
    }

    fun pen(flags: Int, x: Int, y: Int, pressure: Int, tiltX: Int, tiltY: Int, rotation: Int) = put(Protocol.PEN, 11) { b ->
        b.put(flags.toByte())
        b.putShort(x.toShort())
        b.putShort(y.toShort())
        b.putShort(pressure.toShort())
        b.put(tiltX.toByte())
        b.put(tiltY.toByte())
        b.putShort(rotation.toShort())
    }

    fun requestKeyframe() = put(Protocol.REQUEST_KEYFRAME, 0) {}

    fun close() {
        lock.withLock {
            closed = true
            ready.signalAll()
        }
    }

    private inline fun put(type: Int, size: Int, body: (ByteBuffer) -> Unit) {
        lock.withLock {
            val b = front
            if (closed || b.remaining() < size + 5) return
            b.putInt(size + 1)
            b.put(type.toByte())
            body(b)
            ready.signal()
        }
    }

    private fun drain() {
        runCatching { Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY) }
        try {
            while (true) {
                val batch = lock.withLock {
                    while (!closed && front.position() == 0) {
                        if (!ready.await(Protocol.HEARTBEAT_MS, TimeUnit.MILLISECONDS) && !closed && front.position() == 0) {
                            front.putInt(1)
                            front.put(Protocol.HEARTBEAT.toByte())
                        }
                    }
                    if (closed) return
                    val full = front
                    front = back
                    back = full
                    full
                }
                out.write(batch.array(), 0, batch.position())
                out.flush()
                batch.clear()
            }
        } catch (e: IOException) {
        } catch (e: InterruptedException) {
        } finally {
            close()
            runCatching { socket.close() }
        }
    }

    private companion object {
        const val CAPACITY = 256 * 1024
    }
}
