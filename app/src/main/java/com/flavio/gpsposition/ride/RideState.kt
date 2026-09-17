package com.flavio.gpsposition.ride

import com.flavio.gpsposition.location.LocationState
import kotlinx.coroutines.flow.MutableStateFlow

/**
 * Stato condiviso tra il foreground service e la UI dell'Activity.
 */
object RideState {
    val location = MutableStateFlow(LocationState())
    val stats = MutableStateFlow(RideStats())
    val bleStatus = MutableStateFlow("BLE: spento")
    val running = MutableStateFlow(false)

    fun reset() {
        location.value = LocationState()
        stats.value = RideStats()
        bleStatus.value = "BLE: spento"
    }
}
