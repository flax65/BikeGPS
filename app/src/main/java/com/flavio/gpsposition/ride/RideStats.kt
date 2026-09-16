package com.flavio.gpsposition.ride

import com.flavio.gpsposition.location.LocationState
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale
import kotlin.math.asin
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.sin
import kotlin.math.sqrt

/** Statistiche di uscita accumulate durante la pedalata. */
data class RideStats(
    val distanceKm: Double = 0.0,
    val movingSec: Long = 0L,
    val maxSpeedKmh: Float = 0f,
    val slopePct: Float = 0f,
) {
    /** Velocità media in movimento (km/h). */
    val avgSpeedKmh: Float
        get() = if (movingSec > 0) (distanceKm / (movingSec / 3600.0)).toFloat() else 0f

    val movingLabel: String
        get() {
            val h = movingSec / 3600
            val m = (movingSec % 3600) / 60
            val s = movingSec % 60
            return String.format(Locale.US, "%02d:%02d:%02d", h, m, s)
        }
}

/**
 * Accumula distanza, tempo in movimento, velocità max e pendenza a partire dai fix GPS.
 */
class RideCalculator {
    private var last: LocationState? = null
    private var distanceM = 0.0
    private var movingMs = 0L
    private var maxSpeed = 0f
    private var smoothedSlope = 0f

    fun update(s: LocationState): RideStats {
        val prev = last
        if (prev != null && prev.hasFix && s.hasFix) {
            val dt = (s.timeMs - prev.timeMs) / 1000.0
            if (dt in 0.1..10.0) {
                val d = haversine(
                    prev.latitude, prev.longitude,
                    s.latitude, s.longitude
                )
                // scarta i salti assurdi (fix sporchi): max 200 m e max 30 m/s (108 km/h)
                if (d in 0.0..200.0 && d / dt <= 30.0) {
                    distanceM += d
                    if (s.speedMs > 0.5f) movingMs += (dt * 1000).toLong()

                    val a0 = prev.altitude
                    val a1 = s.altitude
                    if (a0 != null && a1 != null && d > 3.0) {
                        val slope = ((a1 - a0) / d * 100.0).toFloat()
                        smoothedSlope = smoothedSlope * 0.7f + slope * 0.3f
                    }
                }
            }
        }
        last = s
        if (s.hasFix) maxSpeed = max(maxSpeed, s.speedKmh)

        return RideStats(
            distanceKm = distanceM / 1000.0,
            movingSec = movingMs / 1000,
            maxSpeedKmh = maxSpeed,
            slopePct = smoothedSlope,
        )
    }

    fun reset() {
        last = null
        distanceM = 0.0
        movingMs = 0L
        maxSpeed = 0f
        smoothedSlope = 0f
    }
}

private const val EARTH_R = 6371000.0

/** Distanza in metri tra due coordinate (formula di Haversine). */
fun haversine(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
    val dLat = Math.toRadians(lat2 - lat1)
    val dLon = Math.toRadians(lon2 - lon1)
    val a = sin(dLat / 2).let { it * it } +
        cos(Math.toRadians(lat1)) * cos(Math.toRadians(lat2)) *
        sin(dLon / 2).let { it * it }
    return 2 * EARTH_R * asin(sqrt(a))
}

/**
 * Costruisce il pacchetto BLE binario compatto (14 byte, entra in MTU 23).
 * Little-endian:
 *   0  u16  velocità         (0.1 km/h)
 *   2  u32  distanza         (0.01 km)
 *   6  u16  tempo movimento  (s)
 *   8  i16  quota            (m)
 *  10  i8   pendenza         (0.5 %)
 *  11  u8   satelliti
 *  12  u16  velocità max     (0.1 km/h)
 */
fun telemetryPayload(s: LocationState, st: RideStats): ByteArray {
    val buf = ByteBuffer.allocate(14).order(ByteOrder.LITTLE_ENDIAN)
    buf.putShort((s.speedKmh * 10f).toInt().coerceIn(0, 65535).toShort())
    buf.putInt((st.distanceKm * 100.0).toLong().coerceIn(0L, 0xFFFFFFFFL).toInt())
    buf.putShort(st.movingSec.coerceIn(0L, 65535L).toShort())
    buf.putShort((s.altitude ?: 0.0).toInt().coerceIn(-32768, 32767).toShort())
    buf.put((st.slopePct * 2f).toInt().coerceIn(-128, 127).toByte())
    buf.put((s.satellitesUsed ?: 0).coerceIn(0, 255).toByte())
    buf.putShort((st.maxSpeedKmh * 10f).toInt().coerceIn(0, 65535).toShort())
    return buf.array()
}
