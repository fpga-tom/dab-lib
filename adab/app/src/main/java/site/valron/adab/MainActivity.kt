package site.valron.adab

import androidx.appcompat.app.AppCompatActivity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Typeface
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import site.valron.adab.databinding.ActivityMainBinding
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var usbManager: UsbManager
    private val scannerExecutor: ExecutorService = Executors.newSingleThreadExecutor()
    private val scanning = AtomicBoolean(false)
    private val services = linkedMapOf<String, ServiceRow>()
    private var pendingScanAfterPermission = false

    private val usbPermissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action != ACTION_USB_PERMISSION) {
                return
            }

            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            val device = currentUsbDevice(intent)
            if (granted && device != null) {
                binding.statusText.text = "RTL-SDR USB permission granted."
                if (pendingScanAfterPermission) {
                    pendingScanAfterPermission = false
                    startNativeScan()
                }
            } else {
                pendingScanAfterPermission = false
                binding.statusText.text = "RTL-SDR USB permission denied."
                setScanningUi(false)
                scanning.set(false)
            }
        }
    }

    private data class ServiceRow(
        val channel: String,
        val frequencyHz: Long,
        val serviceId: Int,
        val label: String
    )

    interface ScanCallback {
        fun onScanProgress(index: Int, total: Int, channel: String, frequencyHz: Long)
        fun onScanLock(index: Int, total: Int, channel: String, frequencyHz: Long, serviceCount: Int, labelCount: Int)
        fun onScanService(channel: String, frequencyHz: Long, serviceId: Int, label: String)
        fun onScanComplete(totalServices: Int)
        fun onScanError(message: String)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        usbManager = getSystemService(USB_SERVICE) as UsbManager
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.statusText.text = stringFromJNI()
        binding.startScanButton.setOnClickListener { startScan() }
        binding.cancelScanButton.setOnClickListener { requestCancel() }
        registerUsbPermissionReceiver()
        refreshUsbStatus()
    }

    override fun onDestroy() {
        requestCancel()
        unregisterReceiver(usbPermissionReceiver)
        scannerExecutor.shutdownNow()
        super.onDestroy()
    }

    private fun startScan() {
        if (!scanning.compareAndSet(false, true)) {
            return
        }

        if (!isRtlsdrLibraryAvailable()) {
            scanning.set(false)
            binding.statusText.text = "Missing librtlsdr.so. Add ABI builds under app/src/main/jniLibs."
            return
        }

        val device = findRtlsdrDevice()
        if (device == null) {
            scanning.set(false)
            binding.statusText.text = "No RTL-SDR USB device found."
            return
        }

        services.clear()
        renderServices()
        setScanningUi(true)
        binding.scanProgress.progress = 0

        if (!usbManager.hasPermission(device)) {
            pendingScanAfterPermission = true
            binding.statusText.text = "Requesting RTL-SDR USB permission..."
            usbManager.requestPermission(device, usbPermissionIntent())
            return
        }

        startNativeScan()
    }

    private fun startNativeScan() {
        binding.statusText.text = "Starting RTL-SDR scan..."

        scannerExecutor.execute {
            val rc = startRtlsdrScan(
                object : ScanCallback {
                    override fun onScanProgress(index: Int, total: Int, channel: String, frequencyHz: Long) {
                        runOnUiThread {
                            binding.scanProgress.max = total
                            binding.scanProgress.progress = index.coerceIn(0, total)
                            binding.statusText.text = "Scanning $channel (${formatFrequency(frequencyHz)}) - $index/$total"
                        }
                    }

                    override fun onScanLock(
                        index: Int,
                        total: Int,
                        channel: String,
                        frequencyHz: Long,
                        serviceCount: Int,
                        labelCount: Int
                    ) {
                        runOnUiThread {
                            binding.scanProgress.max = total
                            binding.scanProgress.progress = index.coerceIn(0, total)
                            binding.statusText.text =
                                "Locked $channel (${formatFrequency(frequencyHz)}): $serviceCount services, $labelCount labels"
                        }
                    }

                    override fun onScanService(channel: String, frequencyHz: Long, serviceId: Int, label: String) {
                        runOnUiThread {
                            val key = "$channel:$frequencyHz:$serviceId"
                            services[key] = ServiceRow(channel, frequencyHz, serviceId, label)
                            renderServices()
                        }
                    }

                    override fun onScanComplete(totalServices: Int) {
                        runOnUiThread {
                            binding.scanProgress.progress = binding.scanProgress.max
                            binding.statusText.text = "Scan complete: $totalServices services found."
                        }
                    }

                    override fun onScanError(message: String) {
                        runOnUiThread {
                            binding.statusText.text = "Scan error: $message"
                        }
                    }
                },
                4
            )

            runOnUiThread {
                scanning.set(false)
                setScanningUi(false)
                if (rc != 0 && !binding.statusText.text.startsWith("Scan error")) {
                    binding.statusText.text = if (rc == -2) {
                        "Scan cancelled."
                    } else {
                        "Scan failed with native status $rc."
                    }
                }
            }
        }
    }

    private fun requestCancel() {
        if (pendingScanAfterPermission) {
            pendingScanAfterPermission = false
            scanning.set(false)
            binding.statusText.text = "Scan cancelled."
            setScanningUi(false)
            return
        }
        if (scanning.get()) {
            cancelScan()
            binding.statusText.text = "Cancelling scan..."
            binding.cancelScanButton.isEnabled = false
        }
    }

    private fun setScanningUi(active: Boolean) {
        binding.startScanButton.isEnabled = !active
        binding.cancelScanButton.isEnabled = active
        binding.scanProgress.isIndeterminate = false
    }

    private fun refreshUsbStatus() {
        val device = findRtlsdrDevice()
        binding.statusText.text = when {
            !isRtlsdrLibraryAvailable() -> "Native RTL-SDR library is not packaged."
            device == null -> "Connect an RTL-SDR USB device to scan."
            usbManager.hasPermission(device) -> "RTL-SDR ready: ${device.deviceName}"
            else -> "RTL-SDR detected. Press Start Scan to grant USB access."
        }
    }

    private fun registerUsbPermissionReceiver() {
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(usbPermissionReceiver, filter, RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(usbPermissionReceiver, filter)
        }
    }

    private fun usbPermissionIntent(): PendingIntent {
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        return PendingIntent.getBroadcast(this, 0, Intent(ACTION_USB_PERMISSION), flags)
    }

    private fun findRtlsdrDevice(): UsbDevice? =
        usbManager.deviceList.values.firstOrNull { isRtlsdrDevice(it) }

    private fun isRtlsdrDevice(device: UsbDevice): Boolean =
        device.vendorId == RTLSDR_VENDOR_ID && device.productId in RTLSDR_PRODUCT_IDS

    private fun currentUsbDevice(intent: Intent): UsbDevice? =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
        } else {
            @Suppress("DEPRECATION")
            intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
        }

    private fun renderServices() {
        binding.servicesContainer.removeAllViews()

        if (services.isEmpty()) {
            binding.servicesContainer.addView(binding.emptyResultsText)
            binding.emptyResultsText.visibility = View.VISIBLE
            return
        }

        for (service in services.values) {
            binding.servicesContainer.addView(createServiceRow(service))
        }
    }

    private fun createServiceRow(service: ServiceRow): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, 14.dp, 0, 14.dp)
            showDividers = LinearLayout.SHOW_DIVIDER_NONE
        }

        val title = TextView(this).apply {
            text = service.label.ifBlank { "Service ${formatServiceId(service.serviceId)}" }
            textSize = 16f
            typeface = Typeface.DEFAULT_BOLD
        }

        val detail = TextView(this).apply {
            text = "${service.channel}  |  ${formatFrequency(service.frequencyHz)}  |  SID ${formatServiceId(service.serviceId)}"
            textSize = 13f
        }

        row.addView(title)
        row.addView(detail)
        return row
    }

    private fun formatFrequency(frequencyHz: Long): String =
        String.format(Locale.ROOT, "%.3f MHz", frequencyHz / 1_000_000.0)

    private fun formatServiceId(serviceId: Int): String =
        "0x${Integer.toUnsignedLong(serviceId).toString(16).padStart(8, '0').uppercase(Locale.ROOT)}"

    private val Int.dp: Int
        get() = (this * resources.displayMetrics.density).toInt()

    /**
     * A native method that is implemented by the 'adab' native library,
     * which is packaged with this application.
     */
    external fun stringFromJNI(): String
    external fun startRtlsdrScan(callback: ScanCallback, perFrequencySeconds: Int): Int
    external fun cancelScan()
    external fun isRtlsdrLibraryAvailable(): Boolean

    companion object {
        private const val ACTION_USB_PERMISSION = "site.valron.adab.USB_PERMISSION"
        private const val RTLSDR_VENDOR_ID = 0x0bda
        private val RTLSDR_PRODUCT_IDS = setOf(
            0x2832,
            0x2838,
            0x2831,
            0x2833,
            0x2834,
            0x2835,
            0x2836,
            0x2837,
            0x2839,
            0x283a,
            0x283b,
            0x283c,
            0x283d,
            0x283e,
            0x283f
        )

        // Used to load the 'adab' library on application startup.
        init {
            System.loadLibrary("adab")
        }
    }
}
