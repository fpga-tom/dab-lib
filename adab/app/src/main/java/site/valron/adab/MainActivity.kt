package site.valron.adab

import androidx.appcompat.app.AppCompatActivity
import android.graphics.Typeface
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
    private val scannerExecutor: ExecutorService = Executors.newSingleThreadExecutor()
    private val scanning = AtomicBoolean(false)
    private val services = linkedMapOf<String, ServiceRow>()

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

        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.statusText.text = stringFromJNI()
        binding.startScanButton.setOnClickListener { startScan() }
        binding.cancelScanButton.setOnClickListener { requestCancel() }
    }

    override fun onDestroy() {
        requestCancel()
        scannerExecutor.shutdownNow()
        super.onDestroy()
    }

    private fun startScan() {
        if (!scanning.compareAndSet(false, true)) {
            return
        }

        services.clear()
        renderServices()
        setScanningUi(true)
        binding.scanProgress.progress = 0
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

    companion object {
        // Used to load the 'adab' library on application startup.
        init {
            System.loadLibrary("adab")
        }
    }
}
