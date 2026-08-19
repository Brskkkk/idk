package com.frameflow

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.media.ImageReader
import android.media.projection.MediaProjectionManager
import android.os.Bundle
import android.util.DisplayMetrics
import android.view.Surface
import android.view.SurfaceView
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {
    private lateinit var surfaceView: SurfaceView
    private lateinit var status: TextView
    private var reader: ImageReader? = null
    private var projection: android.media.projection.MediaProjection? = null
    private var virtualDisplay: android.hardware.display.VirtualDisplay? = null
    private var running = false

    private val capturePermission = registerForActivityResult(ActivityResultContracts.StartActivityForResult()) { result ->
        if (result.resultCode != Activity.RESULT_OK || result.data == null) {
            status.text = "Capture permission denied"
            return@registerForActivityResult
        }
        startCapture(result.data!!)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        System.loadLibrary("frameflow")

        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val bar = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
        val start = Button(this).apply { text = "Start 2×" }
        val stop = Button(this).apply { text = "Stop" }
        status = TextView(this).apply { text = "Ready — Vulkan motion interpolation prototype" }
        bar.addView(start)
        bar.addView(stop)
        root.addView(bar)
        root.addView(status)
        surfaceView = SurfaceView(this)
        root.addView(surfaceView, LinearLayout.LayoutParams(-1, 0, 1f))
        setContentView(root)

        start.setOnClickListener {
            if (!running) {
                val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
                capturePermission.launch(mpm.createScreenCaptureIntent())
            }
        }
        stop.setOnClickListener { stopCapture() }
    }

    private fun startCapture(data: Intent) {
        val metrics = DisplayMetrics()
        windowManager.defaultDisplay.getRealMetrics(metrics)
        val w = metrics.widthPixels
        val h = metrics.heightPixels
        val dpi = metrics.densityDpi

        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        projection = mpm.getMediaProjection(Activity.RESULT_OK, data)
        reader = ImageReader.newInstance(w, h, android.graphics.PixelFormat.RGBA_8888, 3)

        val surface = surfaceView.holder.surface
        if (!nativeInit(surface, w, h)) {
            status.text = "Vulkan initialization failed"
            reader?.close(); reader = null
            projection?.stop(); projection = null
            return
        }

        reader!!.setOnImageAvailableListener({ r ->
            val image = r.acquireLatestImage() ?: return@setOnImageAvailableListener
            try {
                val hb = image.hardwareBuffer
                if (hb != null) {
                    nativeSubmitFrame(hb)
                    hb.close()
                }
            } finally {
                image.close()
            }
        }, null)

        virtualDisplay = projection!!.createVirtualDisplay(
            "FrameFlowCapture", w, h, dpi,
            android.hardware.display.DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
            reader!!.surface, null, null
        )
        running = true
        status.text = "Running — capture ${w}×${h}, GPU motion interpolation"
    }

    private fun stopCapture() {
        running = false
        virtualDisplay?.release(); virtualDisplay = null
        reader?.close(); reader = null
        projection?.stop(); projection = null
        nativeStop()
        status.text = "Stopped"
    }

    override fun onDestroy() {
        stopCapture()
        super.onDestroy()
    }

    private external fun nativeInit(surface: Surface, width: Int, height: Int): Boolean
    private external fun nativeSubmitFrame(buffer: android.hardware.HardwareBuffer)
    private external fun nativeStop()
}
