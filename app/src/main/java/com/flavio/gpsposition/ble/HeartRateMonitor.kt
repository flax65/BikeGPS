package com.flavio.gpsposition.ble

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.ParcelUuid
import android.util.Log
import androidx.core.content.ContextCompat
import com.flavio.gpsposition.ride.HeartRateState
import java.util.UUID

/**
 * Legge un cardiofrequenzimetro BLE **standard** (Heart Rate Service 0x180D),
 * come i Geonaute/Decathlon, Polar, Garmin, Wahoo, ecc.
 *
 * Come [BleMirror] è completamente opzionale e non blocca la registrazione GPS:
 * se il cardio non c'è, si limita a segnalare lo stato e a riprovare.
 */
class HeartRateMonitor(
    context: Context,
    private val onStatus: (String) -> Unit,
    private val onHeartRate: (HeartRateState) -> Unit,
) {
    companion object {
        /** Heart Rate Service (BLE SIG). */
        val HR_SERVICE: UUID = UUID.fromString("0000180d-0000-1000-8000-00805f9b34fb")

        /** Heart Rate Measurement. */
        val HR_MEASUREMENT: UUID = UUID.fromString("00002a37-0000-1000-8000-00805f9b34fb")

        /** Client Characteristic Configuration Descriptor. */
        private val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

        private const val SCAN_TIMEOUT_MS = 15_000L
        private const val RETRY_MS = 5_000L
        private const val RECONNECT_MS = 2_000L
    }

    private val appContext = context.applicationContext
    private val adapter =
        (appContext.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter
    private val scanner get() = adapter?.bluetoothLeScanner
    private val main = Handler(Looper.getMainLooper())

    private var gatt: BluetoothGatt? = null
    private var wantConnected = false
    private var scanning = false

    private val scanTimeout = Runnable {
        if (!scanning) return@Runnable
        stopScan()
        onStatus("Cardio: non trovato")
        retryLater(RETRY_MS)
    }

    fun start() {
        if (adapter == null) {
            onStatus("Cardio: non disponibile")
            return
        }
        wantConnected = true
        startScan()
    }

    fun stop() {
        wantConnected = false
        main.removeCallbacks(scanTimeout)
        stopScan()
        closeGatt()
        onStatus("Cardio: spento")
        onHeartRate(HeartRateState())
    }

    private fun hasPermissions(): Boolean = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        ContextCompat.checkSelfPermission(appContext, Manifest.permission.BLUETOOTH_SCAN) ==
            PackageManager.PERMISSION_GRANTED &&
            ContextCompat.checkSelfPermission(appContext, Manifest.permission.BLUETOOTH_CONNECT) ==
            PackageManager.PERMISSION_GRANTED
    } else {
        true
    }

    @SuppressLint("MissingPermission")
    private fun startScan() {
        if (scanning || !wantConnected) return
        val a = adapter ?: return

        if (!hasPermissions()) {
            onStatus("Cardio: permessi mancanti")
            retryLater(RETRY_MS)
            return
        }
        if (!a.isEnabled) {
            onStatus("Cardio: adattatore spento")
            retryLater(RETRY_MS)
            return
        }
        val s = scanner
        if (s == null) {
            onStatus("Cardio: scanner non disponibile")
            retryLater(RETRY_MS)
            return
        }

        scanning = true
        onStatus("Cardio: ricerca…")
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(HR_SERVICE))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        main.removeCallbacks(scanTimeout)
        runCatching { s.startScan(listOf(filter), settings, scanCallback) }
            .onSuccess { main.postDelayed(scanTimeout, SCAN_TIMEOUT_MS) }
            .onFailure {
                scanning = false
                onStatus("Cardio: scan non avviato")
                retryLater(RETRY_MS)
            }
    }

    @SuppressLint("MissingPermission")
    private fun stopScan() {
        main.removeCallbacks(scanTimeout)
        if (!scanning) return
        scanning = false
        runCatching { scanner?.stopScan(scanCallback) }
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult?) {
            val device = result?.device ?: return
            stopScan()
            connect(device)
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            onStatus("Cardio: scan fallito ($errorCode)")
            retryLater(RETRY_MS)
        }
    }

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        closeGatt()
        onStatus("Cardio: connessione…")
        runCatching {
            gatt = device.connectGatt(appContext, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        }.onFailure { onStatus("Cardio: connessione fallita"); retryLater() }
    }

    @SuppressLint("MissingPermission")
    private fun closeGatt() {
        runCatching { gatt?.close() }
        gatt = null
    }

    private fun retryLater(delayMs: Long = RECONNECT_MS) {
        if (!wantConnected) return
        main.postDelayed({ startScan() }, delayMs)
    }

    private val gattCallback = object : BluetoothGattCallback() {
        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    onStatus("Cardio: connesso, configuro…")
                    runCatching { g.discoverServices() }
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    onStatus("Cardio: disconnesso")
                    closeGatt()
                    retryLater()
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val ch = g.getService(HR_SERVICE)?.getCharacteristic(HR_MEASUREMENT)
            if (ch == null) {
                onStatus("Cardio: servizio non trovato")
                closeGatt()
                retryLater()
                return
            }
            runCatching { g.setCharacteristicNotification(ch, true) }
            val cccd = ch.getDescriptor(CCCD)
            if (cccd == null) {
                onStatus("Cardio: notifiche non disponibili")
                return
            }
            runCatching {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                } else {
                    @Suppress("DEPRECATION")
                    cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    @Suppress("DEPRECATION")
                    g.writeDescriptor(cccd)
                }
            }.onFailure { onStatus("Cardio: errore notifiche") }
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            if (descriptor.uuid == CCCD) onStatus("Cardio: pronto")
        }

        // Android 13+
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            if (ch.uuid == HR_MEASUREMENT) emit(value)
        }

        // Android 12 e precedenti
        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            if (ch.uuid == HR_MEASUREMENT) {
                @Suppress("DEPRECATION")
                emit(ch.value ?: return)
            }
        }
    }

    private fun emit(raw: ByteArray) {
        parse(raw)?.let { onHeartRate(it) }
    }

    /**
     * Decodifica l'Heart Rate Measurement (formato standard BLE SIG).
     * flags bit0: 0 = bpm su 8 bit, 1 = bpm su 16 bit
     * flags bit1: contatto supportato · bit2: contatto rilevato
     * bit3: energia spesa presente · bit4: intervalli RR presenti
     */
    private fun parse(data: ByteArray): HeartRateState? {
        if (data.isEmpty()) return null
        val flags = data[0].toInt() and 0xFF
        val wide = flags and 0x01 != 0

        var i = 1
        val bpm = if (wide) {
            if (data.size < i + 2) return null
            val v = (data[i].toInt() and 0xFF) or ((data[i + 1].toInt() and 0xFF) shl 8)
            i += 2
            v
        } else {
            if (data.size < i + 1) return null
            val v = data[i].toInt() and 0xFF
            i += 1
            v
        }

        val contactSupported = flags and 0x04 != 0
        val contact = if (contactSupported) flags and 0x02 != 0 else true

        Log.d("HeartRate", "bpm=$bpm contact=$contact flags=$flags")
        return HeartRateState(
            bpm = bpm.coerceIn(0, 255),
            contact = contact,
            connected = true,
            timeMs = System.currentTimeMillis(),
        )
    }
}
