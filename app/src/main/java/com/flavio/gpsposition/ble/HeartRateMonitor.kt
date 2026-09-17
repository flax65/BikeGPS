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
        private const val TAG = "HeartRate"
    }

    private val appContext = context.applicationContext
    private val adapter =
        (appContext.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter
    private val scanner get() = adapter?.bluetoothLeScanner
    private val main = Handler(Looper.getMainLooper())

    private var gatt: BluetoothGatt? = null
    private var measurement: BluetoothGattCharacteristic? = null
    private var wantConnected = false
    private var scanning = false
    private var gotNotification = false

    /**
     * Alcune cinture non inviano notifiche (o le interrompono): leggo periodicamente
     * la caratteristica. Tiene vivo il link e recupera comunque il battito.
     */
    private val poll = object : Runnable {
        override fun run() {
            if (!wantConnected || gotNotification) return
            val g = gatt ?: return
            val ch = measurement ?: return
            runCatching { g.readCharacteristic(ch) }
            main.postDelayed(this, 2_000L)
        }
    }

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
        main.removeCallbacks(poll)
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
            @SuppressLint("MissingPermission")
            Log.d(TAG, "trovato ${device.name ?: "?"} (${device.address}) rssi=${result.rssi}")
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
        Log.d(TAG, "connect ${device.name ?: "?"} (${device.address})")
        runCatching {
            gatt = device.connectGatt(appContext, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        }.onFailure { onStatus("Cardio: connessione fallita"); retryLater() }
    }

    @SuppressLint("MissingPermission")
    private fun closeGatt() {
        main.removeCallbacks(poll)
        runCatching { gatt?.close() }
        gatt = null
        measurement = null
        gotNotification = false
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
                    Log.d(TAG, "connected status=$status bond=${g.device.bondState}")
                    onStatus("Cardio: connesso, configuro…")
                    runCatching {
                        g.discoverServices()
                    }
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    Log.d(TAG, "disconnected status=$status bond=${g.device.bondState}")
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
            measurement = ch
            Log.d(TAG, "2A37 properties=0x${Integer.toHexString(ch.properties)}")
            runCatching { g.setCharacteristicNotification(ch, true) }
            val cccd = ch.getDescriptor(CCCD)
            if (cccd == null) {
                Log.d(TAG, "CCCD non trovato")
                onStatus("Cardio: notifiche non disponibili")
                return
            }
            // Scegli il valore giusto: Notify se supportato, altrimenti Indicate.
            val cccdValue = when {
                ch.properties and BluetoothGattCharacteristic.PROPERTY_NOTIFY != 0 ->
                    BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                ch.properties and BluetoothGattCharacteristic.PROPERTY_INDICATE != 0 ->
                    BluetoothGattDescriptor.ENABLE_INDICATION_VALUE
                else -> BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            }
            Log.d(TAG, "CCCD value=${cccdValue.joinToString("") { "%02x".format(it) }}")
            runCatching {
                val ret = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    g.writeDescriptor(cccd, cccdValue)
                } else {
                    @Suppress("DEPRECATION")
                    cccd.value = cccdValue
                    @Suppress("DEPRECATION")
                    g.writeDescriptor(cccd)
                    0
                }
                Log.d(TAG, "writeDescriptor ret=$ret")
                if (ret != 0) onStatus("Cardio: errore notifiche ($ret)")
            }.onFailure { onStatus("Cardio: errore notifiche") }
        }

        override fun onDescriptorWrite(
            g: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            if (descriptor.uuid != CCCD) return
            Log.d(TAG, "onDescriptorWrite status=$status")
            if (status == BluetoothGatt.GATT_SUCCESS) {
                onStatus("Cardio: pronto")
                // fallback: se 2A37 è leggibile, leggo subito e poi periodicamente
                val ch = measurement
                if (ch != null &&
                    ch.properties and BluetoothGattCharacteristic.PROPERTY_READ != 0
                ) {
                    runCatching { g.readCharacteristic(ch) }
                    main.removeCallbacks(poll)
                    main.postDelayed(poll, 1_500L)
                } else {
                    Log.d(TAG, "2A37 senza proprietà READ: solo notifiche")
                }
            } else {
                onStatus("Cardio: notifiche negate ($status)")
            }
        }

        // Android 13+
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            if (ch.uuid == HR_MEASUREMENT) {
                gotNotification = true
                main.removeCallbacks(poll)
                emit(value)
            }
        }

        // Android 12 e precedenti
        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(g: BluetoothGatt, ch: BluetoothGattCharacteristic) {
            if (ch.uuid == HR_MEASUREMENT) {
                @Suppress("DEPRECATION")
                val value = ch.value ?: return
                gotNotification = true
                main.removeCallbacks(poll)
                emit(value)
            }
        }

        // Lettura diretta (fallback): Android 13+
        override fun onCharacteristicRead(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int,
        ) {
            Log.d(TAG, "onCharacteristicRead status=$status size=${value.size}")
            if (ch.uuid == HR_MEASUREMENT && status == BluetoothGatt.GATT_SUCCESS) emit(value)
        }

        // Lettura diretta (fallback): Android 12 e precedenti
        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicRead(
            g: BluetoothGatt,
            ch: BluetoothGattCharacteristic,
            status: Int,
        ) {
            if (ch.uuid == HR_MEASUREMENT && status == BluetoothGatt.GATT_SUCCESS) {
                @Suppress("DEPRECATION")
                val value = ch.value ?: return
                emit(value)
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
