package com.flavio.gpsposition.ride

/**
 * Stato corrente del cardiofrequenzimetro BLE.
 *
 * @param bpm      battiti al minuto (0 = nessun dato)
 * @param contact  true se il sensore rileva il contatto col torace
 * @param connected true se il cardio è connesso
 * @param timeMs   timestamp dell'ultimo campione
 */
data class HeartRateState(
    val bpm: Int = 0,
    val contact: Boolean = false,
    val connected: Boolean = false,
    val timeMs: Long = 0L,
)
