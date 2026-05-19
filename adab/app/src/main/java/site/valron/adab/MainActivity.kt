package site.valron.adab

import androidx.appcompat.app.AppCompatActivity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.SharedPreferences
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.LayerDrawable
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import site.valron.adab.databinding.ActivityMainBinding
import org.json.JSONArray
import org.json.JSONObject
import java.nio.ByteBuffer
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var usbManager: UsbManager
    private lateinit var preferences: SharedPreferences
    private val scannerExecutor: ExecutorService = Executors.newSingleThreadExecutor()
    private val playbackExecutor: ExecutorService = Executors.newSingleThreadExecutor()
    private val scanning = AtomicBoolean(false)
    private val playing = AtomicBoolean(false)
    private val services = linkedMapOf<String, ServiceRow>()
    private var pendingScanAfterPermission = false
    private var pendingPlaybackService: ServiceRow? = null
    private var usbConnection: UsbDeviceConnection? = null
    private var playbackUsbConnection: UsbDeviceConnection? = null
    private var selectedService: ServiceRow? = null
    private var playbackEngine: AacPlaybackEngine? = null
    private var receiverPrimedThisSession = false
    private var retuneService: ServiceRow? = null

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
                    startNativeScan(device)
                } else {
                    pendingPlaybackService?.let { service ->
                        pendingPlaybackService = null
                        binding.root.postDelayed({
                            if (playing.get()) {
                                startNativePlayback(device, service)
                            }
                        }, 300L)
                    }
                }
            } else {
                pendingScanAfterPermission = false
                pendingPlaybackService = null
                binding.statusText.text = "RTL-SDR USB permission denied."
                setScanningUi(false)
                scanning.set(false)
                setPlaybackUi(false)
                playing.set(false)
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

    interface PlaybackCallback {
        fun onPlaybackStatus(message: String)
        fun onPlaybackFormat(sampleRate: Int, channels: Int, asc: ByteArray)
        fun onPlaybackAudio(au: ByteArray)
        fun onPlaybackError(message: String)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        usbManager = getSystemService(USB_SERVICE) as UsbManager
        preferences = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.statusText.text = stringFromJNI()
        binding.startScanButton.setOnClickListener { startScan() }
        binding.cancelScanButton.setOnClickListener { requestCancel() }
        binding.playButton.setOnClickListener { startPlayback() }
        binding.stopButton.setOnClickListener { requestStopPlayback() }
        registerUsbPermissionReceiver()
        loadPersistedServices()
        renderServices()
        refreshUsbStatus()
    }

    override fun onDestroy() {
        requestCancel()
        requestStopPlayback()
        unregisterReceiver(usbPermissionReceiver)
        scannerExecutor.shutdownNow()
        playbackExecutor.shutdownNow()
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
        selectedService = null
        renderServices()
        setPlaybackUi(false)
        setScanningUi(true)
        binding.scanProgress.progress = 0

        if (!usbManager.hasPermission(device)) {
            pendingScanAfterPermission = true
            binding.statusText.text = "Requesting RTL-SDR USB permission..."
            usbManager.requestPermission(device, usbPermissionIntent())
            return
        }

        startNativeScan(device)
    }

    private fun startNativeScan(device: UsbDevice) {
        val connection = usbManager.openDevice(device)
        if (connection == null) {
            scanning.set(false)
            setScanningUi(false)
            binding.statusText.text = "Failed to open RTL-SDR USB device."
            return
        }

        usbConnection = connection
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
                            receiverPrimedThisSession = true
                            persistServices()
                            renderServices()
                        }
                    }

                    override fun onScanComplete(totalServices: Int) {
                        runOnUiThread {
                            receiverPrimedThisSession = true
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
                4,
                connection.fileDescriptor
            )

            runOnUiThread {
                usbConnection?.close()
                usbConnection = null
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

    private fun startPlayback() {
        val service = selectedService
        if (service == null) {
            binding.statusText.text = "Select a service first."
            return
        }
        startPlaybackForService(service)
    }

    private fun startPlaybackForService(service: ServiceRow) {
        if (!playing.compareAndSet(false, true)) {
            return
        }
        if (scanning.get()) {
            playing.set(false)
            binding.statusText.text = "Stop scanning before playback."
            return
        }
        if (!isRtlsdrLibraryAvailable()) {
            playing.set(false)
            binding.statusText.text = "Missing librtlsdr.so. Add ABI builds under app/src/main/jniLibs."
            return
        }

        val device = findRtlsdrDevice()
        if (device == null) {
            playing.set(false)
            binding.statusText.text = "No RTL-SDR USB device found."
            return
        }

        setPlaybackUi(true)
        if (!usbManager.hasPermission(device)) {
            pendingPlaybackService = service
            binding.statusText.text = "Requesting RTL-SDR USB permission..."
            usbManager.requestPermission(device, usbPermissionIntent())
            return
        }

        startNativePlayback(device, service)
    }

    private fun startNativePlayback(device: UsbDevice, service: ServiceRow) {
        val connection = usbManager.openDevice(device)
        if (connection == null) {
            playing.set(false)
            setPlaybackUi(false)
            binding.statusText.text = "Failed to open RTL-SDR USB device."
            return
        }

        playbackUsbConnection = connection
        binding.statusText.text = "Starting ${service.label.ifBlank { formatServiceId(service.serviceId) }}..."

        playbackExecutor.execute {
            val engine = AacPlaybackEngine()
            var activeConnection = connection
            playbackEngine = engine
            if (!receiverPrimedThisSession) {
                runOnUiThread {
                    binding.statusText.text = "Acquiring ${service.channel} before playback..."
                }
                val preflightRc = preflightRtlsdrFrequency(
                    object : ScanCallback {
                        override fun onScanProgress(index: Int, total: Int, channel: String, frequencyHz: Long) {
                            runOnUiThread {
                                binding.statusText.text = "Tuning $channel (${formatFrequency(frequencyHz)})..."
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
                                binding.statusText.text = "Locked $channel: $serviceCount services."
                            }
                        }

                        override fun onScanService(channel: String, frequencyHz: Long, serviceId: Int, label: String) {
                            runOnUiThread {
                                val key = "$channel:$frequencyHz:$serviceId"
                                services[key] = ServiceRow(channel, frequencyHz, serviceId, label)
                                persistServices()
                                renderServices()
                            }
                        }

                        override fun onScanComplete(totalServices: Int) {
                            runOnUiThread {
                                binding.statusText.text = "Acquire complete: $totalServices services."
                            }
                        }

                        override fun onScanError(message: String) {
                            runOnUiThread {
                                binding.statusText.text = "Acquire error: $message"
                            }
                        }
                    },
                    service.channel,
                    service.frequencyHz,
                    activeConnection.fileDescriptor
                )
                activeConnection.close()
                if (preflightRc != 0) {
                    engine.close()
                    playbackEngine = null
                    runOnUiThread {
                        if (playbackUsbConnection === activeConnection) {
                            playbackUsbConnection = null
                        }
                        playing.set(false)
                        setPlaybackUi(false)
                        if (!binding.statusText.text.startsWith("Acquire error")) {
                            binding.statusText.text = "Acquire failed with native status $preflightRc."
                        }
                    }
                    return@execute
                }
                receiverPrimedThisSession = true
                val reopenedConnection = usbManager.openDevice(device)
                if (reopenedConnection == null) {
                    engine.close()
                    playbackEngine = null
                    runOnUiThread {
                        playbackUsbConnection = null
                        playing.set(false)
                        setPlaybackUi(false)
                        binding.statusText.text = "Failed to reopen RTL-SDR USB device."
                    }
                    return@execute
                }
                activeConnection = reopenedConnection
                playbackUsbConnection = activeConnection
            }
            val rc = startRtlsdrPlayback(
                object : PlaybackCallback {
                    override fun onPlaybackStatus(message: String) {
                        runOnUiThread {
                            binding.statusText.text = message
                        }
                    }

                    override fun onPlaybackFormat(sampleRate: Int, channels: Int, asc: ByteArray) {
                        runCatching {
                            engine.configure(sampleRate, channels, asc)
                        }.onFailure { error ->
                            runOnUiThread {
                                binding.statusText.text = "Audio decoder error: ${error.message}"
                            }
                            stopPlayback()
                        }
                    }

                    override fun onPlaybackAudio(au: ByteArray) {
                        runCatching {
                            engine.queueAccessUnit(au)
                        }.onFailure { error ->
                            runOnUiThread {
                                binding.statusText.text = "Audio output error: ${error.message}"
                            }
                            stopPlayback()
                        }
                    }

                    override fun onPlaybackError(message: String) {
                        runOnUiThread {
                            binding.statusText.text = "Playback error: $message"
                        }
                    }
                },
                service.frequencyHz,
                service.serviceId,
                activeConnection.fileDescriptor
            )

            engine.close()
            playbackEngine = null
            activeConnection.close()
            runOnUiThread {
                playbackUsbConnection = null
                playing.set(false)
                setPlaybackUi(false)
                val nextService = retuneService
                retuneService = null
                if (nextService != null) {
                    binding.statusText.text = "Retuning to ${nextService.label.ifBlank { formatServiceId(nextService.serviceId) }}..."
                    startPlaybackForService(nextService)
                    return@runOnUiThread
                }
                if (rc != 0 && !binding.statusText.text.startsWith("Playback error") && !binding.statusText.text.startsWith("Audio")) {
                    binding.statusText.text = if (rc == -2) {
                        "Playback stopped."
                    } else {
                        "Playback failed with native status $rc."
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
            usbConnection?.close()
            usbConnection = null
            binding.statusText.text = "Cancelling scan..."
            binding.cancelScanButton.isEnabled = false
        }
    }

    private fun requestStopPlayback() {
        if (pendingPlaybackService != null) {
            pendingPlaybackService = null
            retuneService = null
            playing.set(false)
            binding.statusText.text = "Playback cancelled."
            setPlaybackUi(false)
            return
        }
        if (playing.get()) {
            retuneService = null
            cancelScan()
            stopPlayback()
            binding.statusText.text = "Stopping playback..."
            binding.stopButton.isEnabled = false
        }
    }

    private fun setScanningUi(active: Boolean) {
        binding.startScanButton.isEnabled = !active
        binding.cancelScanButton.isEnabled = active
        binding.scanProgress.isIndeterminate = false
        binding.playButton.isEnabled = !active && !playing.get() && selectedService != null
    }

    private fun setPlaybackUi(active: Boolean) {
        binding.playButton.isEnabled = !active && !scanning.get() && selectedService != null
        binding.stopButton.isEnabled = active
        binding.startScanButton.isEnabled = !active && !scanning.get()
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
            binding.playButton.isEnabled = false
            return
        }

        for (service in services.values) {
            binding.servicesContainer.addView(createServiceRow(service))
        }
        binding.playButton.isEnabled = selectedService != null && !scanning.get() && !playing.get()
    }

    private fun loadPersistedServices() {
        val raw = preferences.getString(PREF_SERVICES, null) ?: return
        val loaded = runCatching { JSONArray(raw) }.getOrNull() ?: return

        services.clear()
        for (index in 0 until loaded.length()) {
            val item = loaded.optJSONObject(index) ?: continue
            val serviceId = parsePersistedServiceId(item)
            val service = ServiceRow(
                channel = item.optString("channel"),
                frequencyHz = item.optLong("frequencyHz"),
                serviceId = serviceId,
                label = item.optString("label")
            )
            if (service.channel.isNotBlank() && service.frequencyHz > 0 && service.serviceId != 0) {
                services["${service.channel}:${service.frequencyHz}:${service.serviceId}"] = service
            }
        }
    }

    private fun persistServices() {
        val items = JSONArray()
        for (service in services.values) {
            items.put(
                JSONObject()
                    .put("channel", service.channel)
                    .put("frequencyHz", service.frequencyHz)
                    .put("serviceId", formatServiceId(service.serviceId))
                    .put("label", service.label)
            )
        }
        preferences.edit().putString(PREF_SERVICES, items.toString()).apply()
    }

    private fun parsePersistedServiceId(item: JSONObject): Int {
        val raw = item.opt("serviceId") ?: return 0
        return when (raw) {
            is Number -> raw.toInt()
            is String -> {
                val value = raw.trim()
                if (value.startsWith("0x", ignoreCase = true)) {
                    value.substring(2).toLongOrNull(16)?.toInt() ?: 0
                } else {
                    value.toLongOrNull()?.toInt() ?: value.toLongOrNull(16)?.toInt() ?: 0
                }
            }
            else -> 0
        }
    }

    private fun createServiceRow(service: ServiceRow): View {
        val selected = service == selectedService
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(if (selected) 18.dp else 0, 14.dp, 0, 14.dp)
            showDividers = LinearLayout.SHOW_DIVIDER_NONE
            isClickable = true
            background = if (selected) selectedServiceBackground() else ColorDrawable(Color.TRANSPARENT)
            setOnClickListener {
                selectedService = service
                renderServices()
                if (playing.get()) {
                    retuneService = service
                    binding.statusText.text =
                        "Retuning to ${service.label.ifBlank { formatServiceId(service.serviceId) }}..."
                    cancelScan()
                    stopPlayback()
                    binding.stopButton.isEnabled = false
                } else {
                    binding.statusText.text =
                        "Selected ${service.label.ifBlank { formatServiceId(service.serviceId) }} on ${service.channel}."
                }
            }
        }

        val title = TextView(this).apply {
            text = service.label.ifBlank { "Service ${formatServiceId(service.serviceId)}" }
            textSize = 16f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(if (selected) Color.rgb(0, 72, 38) else currentTextColor)
        }

        val detail = TextView(this).apply {
            text = "${service.channel}  |  ${formatFrequency(service.frequencyHz)}  |  SID ${formatServiceId(service.serviceId)}"
            textSize = 13f
            setTextColor(if (selected) Color.rgb(0, 96, 52) else currentTextColor)
        }

        row.addView(title)
        row.addView(detail)
        return row
    }

    private fun selectedServiceBackground(): LayerDrawable {
        val fill = GradientDrawable().apply {
            setColor(Color.rgb(213, 244, 226))
            setStroke(1.dp, Color.rgb(0, 109, 59))
            cornerRadius = 6.dp.toFloat()
        }
        val stripe = ColorDrawable(Color.rgb(0, 109, 59))
        return LayerDrawable(arrayOf(fill, stripe)).apply {
            setLayerInset(1, 0, 0, 0, 0)
            setLayerWidth(1, 6.dp)
        }
    }

    private fun formatFrequency(frequencyHz: Long): String =
        String.format(Locale.ROOT, "%.3f MHz", frequencyHz / 1_000_000.0)

    private fun formatServiceId(serviceId: Int): String =
        "0x${Integer.toUnsignedLong(serviceId).toString(16).padStart(8, '0').uppercase(Locale.ROOT)}"

    private val Int.dp: Int
        get() = (this * resources.displayMetrics.density).toInt()

    private class AacPlaybackEngine {
        private var codec: MediaCodec? = null
        private var audioTrack: AudioTrack? = null
        private var sampleRate = 0
        private var channels = 0
        private var presentationTimeUs = 0L

        fun configure(sampleRate: Int, channels: Int, asc: ByteArray) {
            if (codec != null && this.sampleRate == sampleRate && this.channels == channels) {
                return
            }

            close()
            this.sampleRate = sampleRate
            this.channels = channels.coerceIn(1, 2)
            val format = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, sampleRate, this.channels).apply {
                setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectHE)
                setByteBuffer("csd-0", ByteBuffer.wrap(asc))
            }
            codec = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_AAC).apply {
                configure(format, null, null, 0)
                start()
            }
            presentationTimeUs = 0L
        }

        fun queueAccessUnit(au: ByteArray) {
            val decoder = codec ?: return
            val inputIndex = decoder.dequeueInputBuffer(10_000)
            if (inputIndex >= 0) {
                val input = decoder.getInputBuffer(inputIndex) ?: return
                input.clear()
                input.put(au)
                decoder.queueInputBuffer(inputIndex, 0, au.size, presentationTimeUs, 0)
                presentationTimeUs += 24_000L
            }
            drainDecoder(decoder)
        }

        private fun drainDecoder(decoder: MediaCodec) {
            val info = MediaCodec.BufferInfo()
            while (true) {
                when (val outputIndex = decoder.dequeueOutputBuffer(info, 0)) {
                    MediaCodec.INFO_TRY_AGAIN_LATER -> return
                    MediaCodec.INFO_OUTPUT_FORMAT_CHANGED -> configureAudioTrack(decoder.outputFormat)
                    else -> {
                        if (outputIndex >= 0) {
                            val output = decoder.getOutputBuffer(outputIndex)
                            if (output != null && info.size > 0) {
                                output.position(info.offset)
                                output.limit(info.offset + info.size)
                                val pcm = ByteArray(info.size)
                                output.get(pcm)
                                ensureAudioTrack(decoder.outputFormat).write(pcm, 0, pcm.size)
                            }
                            decoder.releaseOutputBuffer(outputIndex, false)
                        }
                    }
                }
            }
        }

        private fun configureAudioTrack(format: MediaFormat) {
            audioTrack?.release()
            audioTrack = null
            sampleRate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
            channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT).coerceIn(1, 2)
            ensureAudioTrack(format)
        }

        private fun ensureAudioTrack(format: MediaFormat): AudioTrack {
            audioTrack?.let { return it }
            val rate = if (format.containsKey(MediaFormat.KEY_SAMPLE_RATE)) {
                format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
            } else {
                sampleRate
            }
            val count = if (format.containsKey(MediaFormat.KEY_CHANNEL_COUNT)) {
                format.getInteger(MediaFormat.KEY_CHANNEL_COUNT).coerceIn(1, 2)
            } else {
                channels
            }
            val channelMask = if (count == 1) AudioFormat.CHANNEL_OUT_MONO else AudioFormat.CHANNEL_OUT_STEREO
            val minBuffer = AudioTrack.getMinBufferSize(rate, channelMask, AudioFormat.ENCODING_PCM_16BIT)
            val track = AudioTrack.Builder()
                .setAudioAttributes(
                    AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_MEDIA)
                        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                        .build()
                )
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setSampleRate(rate)
                        .setChannelMask(channelMask)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .build()
                )
                .setTransferMode(AudioTrack.MODE_STREAM)
                .setBufferSizeInBytes((minBuffer * 4).coerceAtLeast(rate * count * 2 / 2))
                .build()
            track.play()
            audioTrack = track
            return track
        }

        fun close() {
            runCatching {
                codec?.stop()
            }
            codec?.release()
            codec = null
            audioTrack?.pause()
            audioTrack?.flush()
            audioTrack?.release()
            audioTrack = null
        }
    }

    /**
     * A native method that is implemented by the 'adab' native library,
     * which is packaged with this application.
     */
    external fun stringFromJNI(): String
    external fun startRtlsdrScan(callback: ScanCallback, perFrequencySeconds: Int, usbFileDescriptor: Int): Int
    external fun cancelScan()
    external fun preflightRtlsdrFrequency(callback: ScanCallback, channel: String, frequencyHz: Long, usbFileDescriptor: Int): Int
    external fun startRtlsdrPlayback(callback: PlaybackCallback, frequencyHz: Long, serviceId: Int, usbFileDescriptor: Int): Int
    external fun stopPlayback()
    external fun isRtlsdrLibraryAvailable(): Boolean

    companion object {
        private const val ACTION_USB_PERMISSION = "site.valron.adab.USB_PERMISSION"
        private const val PREFS_NAME = "adab_scan"
        private const val PREF_SERVICES = "services"
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
