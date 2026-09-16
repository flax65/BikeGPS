package com.flavio.gpsposition

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import com.flavio.gpsposition.ride.RideState
import com.flavio.gpsposition.service.RideService
import com.flavio.gpsposition.ui.theme.Accent
import com.flavio.gpsposition.ui.theme.GpsPositionTheme
import com.flavio.gpsposition.ui.theme.Green
import com.flavio.gpsposition.ui.theme.Muted
import java.util.Locale

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            GpsPositionTheme {
                BikeScreen()
            }
        }
    }
}

private fun requiredPermissions(): Array<String> {
    val perms = mutableListOf(
        Manifest.permission.ACCESS_FINE_LOCATION,
        Manifest.permission.ACCESS_COARSE_LOCATION
    )
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        perms += Manifest.permission.BLUETOOTH_SCAN
        perms += Manifest.permission.BLUETOOTH_CONNECT
    }
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        perms += Manifest.permission.POST_NOTIFICATIONS
    }
    return perms.toTypedArray()
}

private fun hasAllPermissions(context: Context): Boolean =
    requiredPermissions().all {
        ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
    }

@Composable
fun BikeScreen() {
    val context = LocalContext.current

    val location by RideState.location.collectAsState()
    val stats by RideState.stats.collectAsState()
    val bleStatus by RideState.bleStatus.collectAsState()
    val running by RideState.running.collectAsState()

    var permissionGranted by remember { mutableStateOf(hasAllPermissions(context)) }

    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) {
        permissionGranted = hasAllPermissions(context)
        if (permissionGranted) RideService.start(context)
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .safeDrawingPadding()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text("BikeGPS", fontSize = 26.sp, fontWeight = FontWeight.Bold, color = Accent)
        Text(
            "Mirror GPS → ESP32 (BLE)",
            color = Muted,
            fontSize = 13.sp
        )

        // --- VELOCITÀ ---
        Card(Modifier.fillMaxWidth()) {
            Column(
                Modifier
                    .fillMaxWidth()
                    .padding(20.dp),
                horizontalAlignment = Alignment.CenterHorizontally
            ) {
                Text("VELOCITÀ", color = Muted, fontSize = 12.sp, letterSpacing = 2.sp)
                Text(
                    text = if (location.hasFix)
                        String.format(Locale.US, "%.1f", location.speedKmh) else "--",
                    fontSize = 64.sp,
                    fontWeight = FontWeight.Bold,
                    color = Green
                )
                Text("km/h", color = Muted)
            }
        }

        // --- STATO ---
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                SectionTitle("STATO")
                InfoRow("Servizio", if (running) "attivo" else "fermo")
                InfoRow("BLE", bleStatus)
                InfoRow("Satelliti", location.satellitesUsed?.toString() ?: "--")
                InfoRow("Provider", location.provider.ifBlank { "--" })
            }
        }

        // --- USCITA ---
        Card(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(16.dp)) {
                SectionTitle("USCITA")
                InfoRow("Distanza", String.format(Locale.US, "%.2f km", stats.distanceKm))
                InfoRow("Tempo in movimento", stats.movingLabel)
                InfoRow("Velocità media", String.format(Locale.US, "%.1f km/h", stats.avgSpeedKmh))
                InfoRow("Velocità max", String.format(Locale.US, "%.1f km/h", stats.maxSpeedKmh))
                InfoRow("Quota", location.altitude?.let { String.format(Locale.US, "%.0f m", it) } ?: "--")
                InfoRow("Pendenza", String.format(Locale.US, "%+.1f %%", stats.slopePct))
            }
        }

        if (!permissionGranted) {
            Card(
                colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.error)
            ) {
                Column(Modifier.padding(16.dp)) {
                    Text("Servono i permessi (posizione, BLE, notifiche)", fontWeight = FontWeight.Bold)
                    Spacer(Modifier.height(8.dp))
                    Button(onClick = { permissionLauncher.launch(requiredPermissions()) }) {
                        Text("Concedi permessi")
                    }
                }
            }
        }

        // --- AZIONI ---
        Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            Button(
                modifier = Modifier.weight(1f),
                onClick = {
                    if (!hasAllPermissions(context)) {
                        permissionLauncher.launch(requiredPermissions())
                    } else {
                        RideService.start(context)
                    }
                },
                enabled = !running
            ) { Text("AVVIA") }

            OutlinedButton(
                modifier = Modifier.weight(1f),
                onClick = { RideService.stop(context) },
                enabled = running
            ) { Text("STOP") }
        }

        Text(
            text = if (running) "Registrazione in corso…" else "Premi AVVIA per iniziare",
            color = if (running) Green else Muted,
            fontSize = 12.sp,
            modifier = Modifier.fillMaxWidth(),
            textAlign = TextAlign.Center
        )
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(
        text = text,
        color = Accent,
        fontSize = 12.sp,
        fontWeight = FontWeight.Bold,
        letterSpacing = 2.sp
    )
    Spacer(Modifier.height(8.dp))
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 6.dp),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(label, color = Muted, fontSize = 15.sp)
        Text(
            value,
            color = MaterialTheme.colorScheme.onSurface,
            fontSize = 15.sp,
            fontFamily = FontFamily.Monospace,
            fontWeight = FontWeight.Medium
        )
    }
    HorizontalDivider(color = Muted.copy(alpha = 0.15f))
}
