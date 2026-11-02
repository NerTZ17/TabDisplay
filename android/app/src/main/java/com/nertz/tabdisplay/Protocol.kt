package com.nertz.tabdisplay

import java.io.IOException

object Protocol {
    const val PORT = 27183
    const val VERSION = 1
    const val MAX_LENGTH = 16 * 1024 * 1024
    val MAGIC = byteArrayOf(0x54, 0x44, 0x53, 0x50)

    const val HELLO = 1
    const val CLIENT_HELLO = 2
    const val STREAM_CONFIG = 3
    const val VIDEO_FRAME = 4
    const val FRAME_ACK = 5
    const val TOUCH = 6
    const val PEN = 7
    const val REQUEST_KEYFRAME = 8
    const val CURSOR_SHAPE = 9
    const val CURSOR_POS = 10

    const val CODEC_HEVC = 1
    const val CODEC_H264 = 2

    const val CAP_HARDWARE = 1
    const val CAP_LOW_LATENCY = 2

    const val FRAME_IDR = 1
    const val FRAME_CODEC_CONFIG = 2

    const val TOUCH_CONTACT = 1
    const val TOUCH_CANCELED = 2

    const val PEN_IN_RANGE = 1
    const val PEN_CONTACT = 2
    const val PEN_BARREL = 4
    const val PEN_ERASER = 8

    fun mime(codec: Int): String? = when (codec) {
        CODEC_HEVC -> "video/hevc"
        CODEC_H264 -> "video/avc"
        else -> null
    }

    fun label(codec: Int): String = when (codec) {
        CODEC_HEVC -> "HEVC"
        CODEC_H264 -> "H.264"
        else -> "codec $codec"
    }
}

class StreamConfig(val codec: Int, val width: Int, val height: Int, val fps: Int, val bitrateKbps: Long)

class PanelInfo(val modeId: Int, val width: Int, val height: Int, val maxRefreshHz: Int)

class ProtocolException(message: String) : IOException(message)
