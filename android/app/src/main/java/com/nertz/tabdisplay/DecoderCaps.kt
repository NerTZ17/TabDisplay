package com.nertz.tabdisplay

import android.media.MediaCodecInfo
import android.media.MediaCodecInfo.CodecCapabilities
import android.media.MediaCodecList
import kotlin.math.floor

class DecoderChoice(
    val codec: Int,
    val name: String,
    val hardware: Boolean,
    val lowLatency: Boolean,
    val maxFps: Int,
    val maxWidth: Int,
    val maxHeight: Int,
) {
    val flags: Int
        get() = (if (hardware) Protocol.CAP_HARDWARE else 0) or (if (lowLatency) Protocol.CAP_LOW_LATENCY else 0)
}

object DecoderCaps {
    fun query(width: Int, height: Int): List<DecoderChoice> {
        val infos = MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
        return listOf(Protocol.CODEC_HEVC, Protocol.CODEC_H264).mapNotNull { best(infos, it, width, height) }
    }

    private fun best(infos: Array<MediaCodecInfo>, codec: Int, width: Int, height: Int): DecoderChoice? {
        val mime = Protocol.mime(codec) ?: return null
        var choice: DecoderChoice? = null
        var bestScore = -1
        for (info in infos) {
            if (info.isEncoder) continue
            val type = info.supportedTypes.firstOrNull { it.equals(mime, ignoreCase = true) } ?: continue
            val caps = runCatching { info.getCapabilitiesForType(type) }.getOrNull() ?: continue
            if (caps.isFeatureRequired(CodecCapabilities.FEATURE_SecurePlayback)) continue
            if (caps.isFeatureRequired(CodecCapabilities.FEATURE_TunneledPlayback)) continue
            val video = caps.videoCapabilities ?: continue
            val sized = runCatching { video.isSizeSupported(width, height) }.getOrDefault(false)
            val maxFps = if (sized) {
                runCatching { floor(video.getSupportedFrameRatesFor(width, height).upper).toInt() }.getOrDefault(0)
            } else {
                0
            }
            val score = (if (info.isHardwareAccelerated) 8 else 0) +
                (if (!info.isSoftwareOnly) 4 else 0) +
                (if (sized) 2 else 0) +
                (if (!info.isAlias) 1 else 0)
            if (score > bestScore) {
                bestScore = score
                choice = DecoderChoice(
                    codec = codec,
                    name = info.name,
                    hardware = info.isHardwareAccelerated,
                    lowLatency = caps.isFeatureSupported(CodecCapabilities.FEATURE_LowLatency),
                    maxFps = maxFps.coerceIn(0, 0xffff),
                    maxWidth = video.supportedWidths.upper.coerceIn(0, 0xffff),
                    maxHeight = video.supportedHeights.upper.coerceIn(0, 0xffff),
                )
            }
        }
        return choice
    }
}
