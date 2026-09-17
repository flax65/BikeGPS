package com.flavio.gpsposition.ride

import com.flavio.gpsposition.location.LocationState
import kotlinx.coroutines.flow.MutableStateFlow

/**
 * Stato condiviso tra il foreground service e la UI dell'Activity.
 */
object RideState {
    val location = MutableStateFlow(LocationState())
    val stats = MutableStateFlow(RideStats())
    val heartRate = MutableStateFlow(HeartRateState())
    val bleStatus = MutableStateFlow("BLE: spento")
    val hrStatus = MutableStateFlow("Cardio: spento")
    val running = MutableStateFlow(false)

    fun reset() {
        location.value = LocationState()
        stats.value = RideStats()
        heartRate.value = HeartRateState()
        bleStatus.value = "BLE: spento"
        hrStatus.value = "Cardio: spento"
    }
}
