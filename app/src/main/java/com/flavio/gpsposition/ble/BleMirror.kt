package com.flavio.gpsposition.ble

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
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
import java.util.UUID

/**
 * Client BLE (central) che cerca l'ESP32 "BikeGPS" e gli invia il pacchetto telemetria
 * scrivendo una caratteristica Write-Without-Response. Si riconnette da solo.
 *
 * IMPORTANTE: è completamente opzionale. Se il Bluetooth è spento, i permessi mancano
 * o l'ESP32 non viene trovato, questa classe si limita a segnalare lo stato e a
 * riprovare in background: non blocca né fa crashare la registrazione GPS.
 */
class BleMirror(
    context: Context,
    private val onStatus: (String) -> Unit,
) {
    companion object {
        val SERVICE_UUID: UUID = UUID.fromString("0000a001-0000-1000-8000-00805f9b34fb")
        val TELEMETRY_UUID: UUID = UUID.fromString("0000a002-0000-1000-8000-00805f9b34fb")

        /** Dopo quanto rinuncio alla scansione corrente e riprovo. */
        private const val SCAN_TIMEOUT_MS = 15_000L

        /** Attesa tra un tentativo e il successivo. */
        private const val RETRY_MS = 5_000L

        /** Attesa prima di ritentare la connessione dopo una disconnessione. */
        private const val RECONNECT_MS = 2_000L
    }

    private val appContext = context.applicationContext
    private val adapter =
        (appContext.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter
    private val scanner get() = adapter?.bluetoothLeScanner
    private val main = Handler(Looper.getMainLooper())

    private var gatt: BluetoothGatt? = null
    private var telemetry: BluetoothGattCharacteristic? = null
    private var wantConnected = false
    private var scanning = false
    private var mtu = 23
    private var pending: ByteArray? = null

    /** Chiamato quando la scansione va in timeout: nessun ESP32 trovato. */
    private val scanTimeout = Runnable {
        if (!scanning) return@Runnable
        stopScan()
        onStatus("BLE: ESP32 non trovato")
        retryLater(RETRY_MS)
    }

    /** Avvia il mirror BLE. Non lancia eccezioni: ogni errore diventa uno stato. */
    fun start() {
        if (adapter == null) {
            onStatus("BLE: non disponibile")
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
        onStatus("BLE: spento")
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

        // I permessi possono arrivare dopo l'avvio: in tal caso riprovo, così il BLE
        // parte da solo appena l'utente li concede (senza riavviare il servizio).
        if (!hasPermissions()) {
            onStatus("BLE: permessi mancanti")
            retryLater(RETRY_MS)
            return
        }
        if (!a.isEnabled) {
            onStatus("BLE: adattatore spento")
            retryLater(RETRY_MS)
            return
        }

        val s = scanner
        if (s == null) {
            onStatus("BLE: scanner non disponibile")
            retryLater(RETRY_MS)
            return
        }

        scanning = true
        onStatus("BLE: ricerca ESP32…")
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(SERVICE_UUID))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        main.removeCallbacks(scanTimeout)
        runCatching { s.startScan(listOf(filter), settings, scanCallback) }
            .onSuccess { main.postDelayed(scanTimeout, SCAN_TIMEOUT_MS) }
            .onFailure {
                scanning = false
                onStatus("BLE: scan non avviato")
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
            onStatus("BLE: scan fallito ($errorCode)")
            retryLater(RETRY_MS)
        }
    }

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        closeGatt()
        onStatus("BLE: connessione…")
        runCatching {
            gatt = device.connectGatt(appContext, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
        }.onFailure { onStatus("BLE: connessione fallita"); retryLater() }
    }

    @SuppressLint("MissingPermission")
    private fun closeGatt() {
        runCatching { gatt?.close() }
        gatt = null
        telemetry = null
        mtu = 23
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
                    onStatus("BLE: connesso, configuro…")
                    runCatching {
                        g.discoverServices()
                        g.requestMtu(64)
                    }
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    onStatus("BLE: disconnesso")
                    closeGatt()
                    retryLater()
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            mtu = newMtu
            Log.d("BleMirror", "onMtuChanged mtu=$newMtu status=$status")
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val ch = g.getService(SERVICE_UUID)?.getCharacteristic(TELEMETRY_UUID)
            if (ch == null) {
                onStatus("BLE: servizio non trovato")
                closeGatt()
                retryLater()
            } else {
                telemetry = ch
                onStatus("BLE: pronto")
                Log.d("BleMirror", "servizio trovato, mtu=$mtu")
                flush()
            }
        }
    }

    /** Mette in coda il pacchetto e prova a inviarlo. Se il BLE non c'è, lo scarta. */
    fun send(payload: ByteArray) {
        if (!wantConnected) return
        pending = payload
        flush()
    }

    @SuppressLint("MissingPermission")
    private fun flush() {
        if (!wantConnected) return
        val g = gatt ?: return
        val ch = telemetry ?: return
        val data = pending ?: return
        if (data.size > mtu - 3) {
            Log.d("BleMirror", "drop payload size=${data.size} mtu=$mtu")
            return
        }

        pending = null
        runCatching {
            val result = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeCharacteristic(
                    ch, data, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                ) == BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                @Suppress("DEPRECATION")
                ch.value = data
                @Suppress("DEPRECATION")
                g.writeCharacteristic(ch)
            }
            Log.d("BleMirror", "write size=${data.size} mtu=$mtu ok=$result")
        }
    }
}
