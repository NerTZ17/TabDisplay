package com.nertz.tabdisplay

import android.graphics.Bitmap
import android.os.Process
import android.util.Log
import java.io.BufferedInputStream
import java.io.EOFException
import java.io.IOException
import java.io.InputStream
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder

class StreamServer(
    private val panel: PanelInfo,
    private val decoder: VideoDecoder,
    private val input: InputSender,
    private val stats: Stats,
    private val listener: Listener,
) {
    interface Listener {
        fun onStreamConfig(config: StreamConfig)
        fun onCursorShape(bitmap: Bitmap, hotX: Int, hotY: Int)
        fun onCursorPos(x: Int, y: Int, visible: Boolean)
        fun onSessionEnded()
    }

    @Volatile private var running = true
    @Volatile private var server: ServerSocket? = null
    @Volatile private var client: Socket? = null
    @Volatile private var sessionThread: Thread? = null
    private val thread = Thread(::run, "tabdisplay-accept")

    private val header = ByteArray(5)
    private var buf = ByteArray(1 shl 20)
    private var bb = ByteBuffer.wrap(buf).order(ByteOrder.LITTLE_ENDIAN)
    private var payloadLength = 0
    private var receiveNanos = 0L
    private var codecs: List<DecoderChoice> = emptyList()

    fun start() = thread.start()

    fun stop() {
        running = false
        runCatching { server?.close() }
        runCatching { client?.close() }
    }

    private fun run() {
        runCatching { Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY) }
        codecs = runCatching { DecoderCaps.query(panel.width, panel.height) }.getOrDefault(emptyList())
        decoder.choices = codecs
        for (c in codecs) Log.i(TAG, "decoder ${Protocol.label(c.codec)} ${c.name} flags=${c.flags} maxFps=${c.maxFps} max=${c.maxWidth}x${c.maxHeight}")
        while (running) {
            val ss = try {
                bind()
            } catch (e: IOException) {
                Log.w(TAG, "bind failed", e)
                try {
                    Thread.sleep(1000)
                } catch (ie: InterruptedException) {
                    return
                }
                continue
            }
            server = ss
            if (!running) {
                runCatching { ss.close() }
                return
            }
            try {
                while (running) {
                    val socket = ss.accept()
                    replaceSession(socket)
                }
            } catch (e: IOException) {
                if (running) Log.w(TAG, "accept failed", e)
            } finally {
                runCatching { ss.close() }
            }
        }
    }

    private fun replaceSession(socket: Socket) {
        val previous = sessionThread
        if (previous != null && previous.isAlive) {
            Log.i(TAG, "new host connected, closing previous session")
            runCatching { client?.close() }
            previous.join(SESSION_JOIN_MS)
        }
        if (!running) {
            runCatching { socket.close() }
            return
        }
        client = socket
        sessionThread = Thread({
            runCatching { Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_DISPLAY) }
            session(socket)
        }, "tabdisplay-session").also { it.start() }
    }

    private fun bind(): ServerSocket {
        val ss = ServerSocket()
        try {
            ss.reuseAddress = true
            ss.receiveBufferSize = 4 shl 20
            ss.bind(InetSocketAddress(InetAddress.getByName("127.0.0.1"), Protocol.PORT), 4)
        } catch (e: IOException) {
            runCatching { ss.close() }
            throw e
        }
        return ss
    }

    private fun session(socket: Socket) {
        var sender: Sender? = null
        try {
            socket.tcpNoDelay = true
            socket.soTimeout = Protocol.TIMEOUT_MS
            socket.receiveBufferSize = 4 shl 20
            socket.sendBufferSize = 256 shl 10
            val stream = BufferedInputStream(socket.getInputStream(), 256 shl 10)
            if (readMessage(stream) != Protocol.HELLO) throw ProtocolException("expected Hello")
            need(6)
            for (i in 0 until 4) if (buf[i] != Protocol.MAGIC[i]) throw ProtocolException("bad magic")
            val version = u16(4)
            if (version != Protocol.VERSION) throw ProtocolException("unsupported version $version")
            val s = Sender(socket)
            sender = s
            decoder.sender = s
            stats.resetDrops()
            s.clientHello(panel, codecs)
            Log.i(TAG, "host connected")
            var streaming = false
            while (running) {
                when (readMessage(stream)) {
                    Protocol.STREAM_CONFIG -> {
                        need(11)
                        val cfg = StreamConfig(buf[0].toInt() and 0xff, u16(1), u16(3), u16(5), u32(7))
                        if (Protocol.mime(cfg.codec) == null || cfg.width == 0 || cfg.height == 0) {
                            throw ProtocolException("bad StreamConfig")
                        }
                        Log.i(TAG, "stream ${Protocol.label(cfg.codec)} ${cfg.width}x${cfg.height}@${cfg.fps} ${cfg.bitrateKbps} kbps")
                        listener.onStreamConfig(cfg)
                        decoder.start(cfg)
                        input.sender = s
                        streaming = true
                    }
                    Protocol.VIDEO_FRAME -> if (streaming) {
                        need(13)
                        decoder.feed(buf, 13, payloadLength - 13, u32(0), bb.getLong(4), buf[12].toInt() and 0xff, receiveNanos)
                    }
                    Protocol.CURSOR_SHAPE -> if (streaming) cursorShape()
                    Protocol.CURSOR_POS -> if (streaming) {
                        need(5)
                        listener.onCursorPos(bb.getShort(0).toInt(), bb.getShort(2).toInt(), buf[4].toInt() != 0)
                    }
                }
            }
        } catch (e: IOException) {
            Log.i(TAG, "session ended: $e")
        } catch (e: RuntimeException) {
            Log.w(TAG, "session failed", e)
        } finally {
            input.sender = null
            decoder.stop()
            decoder.sender = null
            sender?.close()
            runCatching { socket.close() }
            if (client === socket) client = null
            listener.onSessionEnded()
        }
    }

    private fun cursorShape() {
        need(8)
        val w = u16(0)
        val h = u16(2)
        need(8L + w.toLong() * h * 4)
        if (w == 0 || h == 0) return
        val pixels = IntArray(w * h)
        for (i in pixels.indices) pixels[i] = bb.getInt(8 + i * 4)
        listener.onCursorShape(Bitmap.createBitmap(pixels, w, h, Bitmap.Config.ARGB_8888), u16(4), u16(6))
    }

    private fun readMessage(stream: InputStream): Int {
        readFully(stream, header, 5)
        val length = (header[0].toLong() and 0xff) or
            ((header[1].toLong() and 0xff) shl 8) or
            ((header[2].toLong() and 0xff) shl 16) or
            ((header[3].toLong() and 0xff) shl 24)
        if (length == 0L || length > Protocol.MAX_LENGTH) throw ProtocolException("bad length $length")
        payloadLength = (length - 1).toInt()
        if (payloadLength > buf.size) {
            buf = ByteArray(maxOf(payloadLength, minOf(buf.size * 2, Protocol.MAX_LENGTH)))
            bb = ByteBuffer.wrap(buf).order(ByteOrder.LITTLE_ENDIAN)
        }
        readFully(stream, buf, payloadLength)
        receiveNanos = System.nanoTime()
        stats.onBytes(length + 4)
        return header[4].toInt() and 0xff
    }

    private fun readFully(stream: InputStream, target: ByteArray, count: Int) {
        var off = 0
        while (off < count) {
            val r = stream.read(target, off, count - off)
            if (r < 0) throw EOFException()
            off += r
        }
    }

    private fun need(bytes: Int) = need(bytes.toLong())

    private fun need(bytes: Long) {
        if (payloadLength < bytes) throw ProtocolException("short payload")
    }

    private fun u16(offset: Int): Int = bb.getShort(offset).toInt() and 0xffff

    private fun u32(offset: Int): Long = bb.getInt(offset).toLong() and 0xffffffffL

    private companion object {
        const val TAG = "TabDisplay"
        const val SESSION_JOIN_MS = 3000L
    }
}
