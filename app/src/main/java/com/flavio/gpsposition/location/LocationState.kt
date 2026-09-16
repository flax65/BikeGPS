package com.flavio.gpsposition.location

/**
 * Stato corrente della posizione GPS.
 */
data class LocationState(
    val hasFix: Boolean = false,
    val latitude: Double = 0.0,
    val longitude: Double = 0.0,
    val altitude: Double? = null,
    val speedMs: Float = 0f,
    val accuracyM: Float = 0f,
    val bearingDeg: Float? = null,
    val provider: String = "",
    val timeMs: Long = 0L,
    val satellitesUsed: Int? = null,
) {
    /** Velocità in km/h. */
    val speedKmh: Float get() = speedMs * 3.6f

    /** Velocità in nodi. */
    val speedKnots: Float get() = speedMs * 1.9438445f
}
