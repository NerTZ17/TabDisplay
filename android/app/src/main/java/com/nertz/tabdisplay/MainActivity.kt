package com.nertz.tabdisplay

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Typeface
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Display
import android.view.Gravity
import android.view.KeyEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.TextView
import android.window.OnBackInvokedDispatcher
import java.util.Locale
import kotlin.math.roundToInt

class MainActivity : Activity(), SurfaceHolder.Callback, StreamServer.Listener {
    private val main = Handler(Looper.getMainLooper())
    private val stats = Stats()
    private lateinit var root: FrameLayout
    private lateinit var surfaceView: SurfaceView
    private lateinit var statsView: TextView
    private lateinit var statusView: TextView
    private lateinit var cursor: CursorOverlay
    private lateinit var decoder: VideoDecoder
    private lateinit var input: InputSender
    private lateinit var server: StreamServer
    private lateinit var panel: PanelInfo
    private var config: StreamConfig? = null
    private var codecLabel = ""

    private val statsTick = object : Runnable {
        override fun run() {
            stats.tick()
            updateStats()
            main.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (Build.VERSION.SDK_INT < 35) edgeToEdgeLegacy()
        val d = checkNotNull(display)
        val mode = pickMode(d)
        window.attributes = window.attributes.also {
            it.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
            it.preferredDisplayModeId = mode.modeId
        }
        val maxRefresh = d.supportedModes
            .filter { it.physicalWidth == mode.physicalWidth && it.physicalHeight == mode.physicalHeight }
            .maxOf { it.refreshRate }
        panel = PanelInfo(
            modeId = mode.modeId,
            width = maxOf(mode.physicalWidth, mode.physicalHeight),
            height = minOf(mode.physicalWidth, mode.physicalHeight),
            maxRefreshHz = maxRefresh.roundToInt(),
        )

        buildViews()
        setContentView(root)
        hideSystemBars()

        decoder = VideoDecoder(stats)
        input = InputSender(surfaceView, main)
        server = StreamServer(panel, decoder, input, stats, this)
        server.start()
        main.post(statsTick)

        if (Build.VERSION.SDK_INT >= 33) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT) { finish() }
        }
    }

    override fun onDestroy() {
        main.removeCallbacksAndMessages(null)
        server.stop()
        decoder.shutdown()
        super.onDestroy()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemBars()
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN) {
            if (event.repeatCount == 0) toggleStats()
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN) return true
        return super.onKeyUp(keyCode, event)
    }

    @Suppress("OVERRIDE_DEPRECATION", "GestureBackNavigation")
    override fun onBackPressed() {
        finish()
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        decoder.attachSurface(holder.surface)
        applyFrameRate()
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        decoder.detachSurface()
    }

    override fun onStreamConfig(config: StreamConfig) {
        main.post { applyConfig(config) }
    }

    override fun onCursorShape(bitmap: Bitmap, hotX: Int, hotY: Int) {
        cursor.postShape(bitmap, hotX, hotY)
    }

    override fun onCursorPos(x: Int, y: Int, visible: Boolean) {
        cursor.postPos(x, y, visible)
    }

    override fun onSessionEnded() {
        main.post {
            config = null
            codecLabel = ""
            cursor.clear()
            surfaceView.visibility = View.GONE
            layoutVideo()
            statusView.visibility = View.VISIBLE
            updateStats()
        }
    }

    private fun buildViews() {
        root = FrameLayout(this)
        root.setBackgroundColor(Color.BLACK)

        surfaceView = SurfaceView(this)
        surfaceView.visibility = View.GONE
        surfaceView.holder.addCallback(this)
        root.addView(surfaceView, FrameLayout.LayoutParams(MATCH, MATCH, Gravity.CENTER))

        val cursorImage = ImageView(this)
        root.addView(cursorImage, FrameLayout.LayoutParams(1, 1, Gravity.TOP or Gravity.START))
        cursor = CursorOverlay(cursorImage, main)

        statusView = TextView(this)
        statusView.text = getString(R.string.waiting, Protocol.PORT)
        statusView.setTextColor(0xFFB8B8B8.toInt())
        statusView.textSize = 22f
        root.addView(statusView, FrameLayout.LayoutParams(WRAP, WRAP, Gravity.CENTER))

        statsView = TextView(this)
        statsView.typeface = Typeface.MONOSPACE
        statsView.textSize = 13f
        statsView.setTextColor(Color.WHITE)
        statsView.setBackgroundColor(0xA0000000.toInt())
        val pad = dp(10)
        statsView.setPadding(pad, pad, pad, pad)
        root.addView(statsView, FrameLayout.LayoutParams(WRAP, WRAP, Gravity.TOP or Gravity.START).also {
            it.setMargins(dp(16), dp(16), 0, 0)
        })

        root.addOnLayoutChangeListener { _, l, t, r, b, oldL, oldT, oldR, oldB ->
            if (r - l != oldR - oldL || b - t != oldB - oldT) root.post { layoutVideo() }
        }
        surfaceView.addOnLayoutChangeListener { v, _, _, _, _, _, _, _, _ ->
            cursor.setVideoRect(v.left, v.top, v.width, config?.width ?: 0)
        }
    }

    private fun applyConfig(cfg: StreamConfig) {
        config = cfg
        val name = decoder.choices.firstOrNull { it.codec == cfg.codec }?.name ?: "default"
        codecLabel = "${Protocol.label(cfg.codec)} $name"
        statusView.visibility = View.GONE
        layoutVideo()
        surfaceView.visibility = View.VISIBLE
        cursor.setVideoRect(surfaceView.left, surfaceView.top, surfaceView.width, cfg.width)
        applyFrameRate()
        updateStats()
    }

    private fun layoutVideo() {
        val cfg = config
        val rw = root.width
        val rh = root.height
        var w = MATCH
        var h = MATCH
        if (cfg != null && rw > 0 && rh > 0) {
            if (rw.toLong() * cfg.height <= rh.toLong() * cfg.width) {
                w = rw
                h = ((rw.toLong() * cfg.height + cfg.width / 2) / cfg.width).toInt()
            } else {
                h = rh
                w = ((rh.toLong() * cfg.width + cfg.height / 2) / cfg.height).toInt()
            }
        }
        val lp = surfaceView.layoutParams
        if (lp.width != w || lp.height != h) {
            lp.width = w
            lp.height = h
            surfaceView.layoutParams = lp
        }
    }

    private fun applyFrameRate() {
        val cfg = config ?: return
        if (cfg.fps <= 0) return
        val s = surfaceView.holder.surface
        if (!s.isValid) return
        runCatching {
            if (Build.VERSION.SDK_INT >= 31) {
                s.setFrameRate(cfg.fps.toFloat(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, Surface.CHANGE_FRAME_RATE_ALWAYS)
            } else {
                s.setFrameRate(cfg.fps.toFloat(), Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
            }
        }
    }

    @Suppress("DEPRECATION")
    private fun edgeToEdgeLegacy() {
        window.setDecorFitsSystemWindows(false)
    }

    private fun hideSystemBars() {
        window.insetsController?.let {
            it.hide(WindowInsets.Type.systemBars())
            it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }

    private fun toggleStats() {
        statsView.visibility = if (statsView.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        if (statsView.visibility == View.VISIBLE) updateStats()
    }

    private fun updateStats() {
        if (statsView.visibility != View.VISIBLE) return
        val current = display?.mode
        val panelLine = String.format(
            Locale.US,
            "panel %dx%d %.0f Hz (max %d)",
            panel.width,
            panel.height,
            current?.refreshRate ?: 0f,
            panel.maxRefreshHz,
        )
        val cfg = config
        statsView.text = if (cfg == null) {
            "idle\n$panelLine"
        } else {
            String.format(
                Locale.US,
                "%dx%d %s\nconfig %d fps  %d kbps\nrender %.1f fps  decode %.2f ms\nrecv %.1f Mbps  drops %d\n%s",
                cfg.width,
                cfg.height,
                codecLabel,
                cfg.fps,
                cfg.bitrateKbps,
                stats.fps,
                stats.decodeMs,
                stats.mbps,
                stats.drops,
                panelLine,
            )
        }
    }

    private fun pickMode(d: Display): Display.Mode {
        var best = d.mode
        for (m in d.supportedModes) {
            val area = m.physicalWidth.toLong() * m.physicalHeight
            val bestArea = best.physicalWidth.toLong() * best.physicalHeight
            if (area > bestArea || (area == bestArea && m.refreshRate > best.refreshRate)) best = m
        }
        return best
    }

    private fun dp(value: Int): Int = (value * resources.displayMetrics.density).roundToInt()

    private companion object {
        const val MATCH = ViewGroup.LayoutParams.MATCH_PARENT
        const val WRAP = ViewGroup.LayoutParams.WRAP_CONTENT
    }
}
