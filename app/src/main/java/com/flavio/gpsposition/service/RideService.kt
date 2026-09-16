package com.flavio.gpsposition.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import com.flavio.gpsposition.R
import com.flavio.gpsposition.ble.BleMirror
import com.flavio.gpsposition.location.GpsTracker
import com.flavio.gpsposition.location.LocationState
import com.flavio.gpsposition.ride.RideCalculator
import com.flavio.gpsposition.ride.RideState
import com.flavio.gpsposition.ride.RideStats
import com.flavio.gpsposition.ride.telemetryPayload
import java.util.Locale

/**
 * Foreground service: continua a leggere il GPS e a inviare i dati all'ESP32
 * anche con lo schermo spento (telefono in tasca).
 */
class RideService : Service() {

    companion object {
        private const val ACTION_START = "com.flavio.gpsposition.action.START"
        private const val ACTION_STOP = "com.flavio.gpsposition.action.STOP"
        private const val CHANNEL_ID = "ride"
        private const val NOTIF_ID = 42

        fun start(context: Context) {
            val intent = Intent(context, RideService::class.java).setAction(ACTION_START)
            ContextCompat.startForegroundService(context, intent)
        }

        fun stop(context: Context) {
            val intent = Intent(context, RideService::class.java).setAction(ACTION_STOP)
            context.startService(intent)
        }
    }

    private lateinit var tracker: GpsTracker
    private lateinit var ble: BleMirror
    private val calc = RideCalculator()
    private var running = false

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        createChannel()
        tracker = GpsTracker(this) { onLocation(it) }
        ble = BleMirror(this) { status -> RideState.bleStatus.value = status }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> {
                stopEverything()
                stopSelf()
                return START_NOT_STICKY
            }
            else -> startEverything()
        }
        return START_STICKY
    }

    private fun startEverything() {
        if (running) return
        running = true
        calc.reset()
        RideState.reset()
        RideState.running.value = true
        startForegroundCompat()
        tracker.start()
        ble.start()
    }

    private fun stopEverything() {
        if (!running) return
        running = false
        tracker.stop()
        ble.stop()
        RideState.running.value = false
    }

    private fun onLocation(s: LocationState) {
        val stats = calc.update(s)
        RideState.location.value = s
        RideState.stats.value = stats
        ble.send(telemetryPayload(s, stats))
        notifyNotification(s, stats)
    }

    // --- Notifica ---

    private fun startForegroundCompat() {
        val notification = buildNotification(LocationState(), RideStats())
        val type = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q)
            ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION else 0
        ServiceCompat.startForeground(this, NOTIF_ID, notification, type)
    }

    private fun notifyNotification(s: LocationState, st: RideStats) {
        val nm = getSystemService(NotificationManager::class.java) ?: return
        runCatching { nm.notify(NOTIF_ID, buildNotification(s, st)) }
    }

    private fun buildNotification(s: LocationState, st: RideStats): Notification {
        val speed = if (s.hasFix) String.format(Locale.US, "%.1f km/h", s.speedKmh) else "in attesa GPS"
        val content = String.format(
            Locale.US,
            "%s · %.2f km · %s",
            speed, st.distanceKm, st.movingLabel
        )
        return NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("BikeGPS attivo")
            .setContentText(content)
            .setSmallIcon(android.R.drawable.ic_menu_mylocation)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                getString(R.string.app_name),
                NotificationManager.IMPORTANCE_LOW
            )
            getSystemService(NotificationManager::class.java)?.createNotificationChannel(channel)
        }
    }
}
