package com.flavio.gpsposition.location

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.location.GnssStatus
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Bundle
import android.os.Looper
import androidx.core.content.ContextCompat

/**
 * Legge posizione, velocità e satelliti DIRETTAMENTE dal GPS hardware del telefono
 * usando solo le API native di Android (android.location), senza Google Play Services.
 *
 * - Provider: GPS_PROVIDER (il GPS satellitare del dispositivo)
 * - Callback: LocationListener
 * - Satelliti: GnssStatus.Callback
 *
 * La velocità è filtrata (mediana + media esponenziale) per eliminare il rumore.
 */
class GpsTracker(
    private val context: Context,
    private val onUpdate: (LocationState) -> Unit,
) {
    private val lm = context.getSystemService(Context.LOCATION_SERVICE) as LocationManager
    private val executor = ContextCompat.getMainExecutor(context)

    private var last: Location? = null
    private var satellites: Int? = null
    private var running = false

    // filtro velocità
    private val speedWindow = ArrayDeque<Float>()
    private var smoothedSpeedMs = 0f
    private var speedInit = false

    private val listener = object : LocationListener {
        override fun onLocationChanged(location: Location) {
            onUpdate(toState(location))
            last = location
        }

        @Deprecated("Deprecated in API 30")
        override fun onStatusChanged(provider: String?, status: Int, extras: Bundle?) {}

        override fun onProviderEnabled(provider: String) {}

        override fun onProviderDisabled(provider: String) {}
    }

    private val gnssCallback = object : GnssStatus.Callback() {
        override fun onSatelliteStatusChanged(status: GnssStatus) {
            var used = 0
            for (i in 0 until status.satelliteCount) {
                if (status.usedInFix(i)) used++
            }
            satellites = used
        }
    }

    val isGpsEnabled: Boolean
        get() = lm.isProviderEnabled(LocationManager.GPS_PROVIDER)

    @SuppressLint("MissingPermission")
    fun start(intervalMs: Long = 1000L) {
        if (running || !hasPermission()) return
        if (!lm.isProviderEnabled(LocationManager.GPS_PROVIDER)) return
        running = true

        // GPS hardware del telefono (provider satellitare).
        lm.requestLocationUpdates(
            LocationManager.GPS_PROVIDER,
            intervalMs,
            0f,
            listener,
            Looper.getMainLooper()
        )

        // Stato dei satelliti (quanti usati nel fix).
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            lm.registerGnssStatusCallback(executor, gnssCallback)
        } else {
            @Suppress("DEPRECATION")
            lm.registerGnssStatusCallback(gnssCallback)
        }
    }

    fun stop() {
        if (!running) return
        running = false
        lm.removeUpdates(listener)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            lm.unregisterGnssStatusCallback(gnssCallback)
        } else {
            @Suppress("DEPRECATION")
            lm.unregisterGnssStatusCallback(gnssCallback)
        }
    }

    private fun hasPermission(): Boolean =
        ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED

    private fun toState(loc: Location): LocationState {
        // Velocità istantanea: preferisci quella del fix (Doppler, affidabile);
        // il calcolo distanza/tempo solo come fallback e comunque limitato.
        val raw = when {
            loc.hasSpeed() -> loc.speed
            else -> {
                val prev = last
                if (prev != null) {
                    val dt = (loc.time - prev.time) / 1000.0
                    if (dt > 0.2) (prev.distanceTo(loc) / dt).toFloat() else 0f
                } else 0f
            }
        }.coerceIn(0f, 28f) // max 100 km/h (bici)

        // filtro: mediana su 3 campioni (uccide i picchi isolati) + media esponenziale (leviga)
        speedWindow.addLast(raw)
        if (speedWindow.size > 3) speedWindow.removeFirst()
        val median = speedWindow.sorted()[speedWindow.size / 2]
        smoothedSpeedMs = if (!speedInit) {
            speedInit = true
            median
        } else {
            smoothedSpeedMs * 0.55f + median * 0.45f
        }

        return LocationState(
            hasFix = true,
            latitude = loc.latitude,
            longitude = loc.longitude,
            altitude = if (loc.hasAltitude()) loc.altitude else null,
            speedMs = smoothedSpeedMs,
            accuracyM = if (loc.hasAccuracy()) loc.accuracy else 0f,
            bearingDeg = if (loc.hasBearing()) loc.bearing else null,
            provider = loc.provider ?: "",
            timeMs = loc.time,
            satellitesUsed = satellites,
        )
    }
}
