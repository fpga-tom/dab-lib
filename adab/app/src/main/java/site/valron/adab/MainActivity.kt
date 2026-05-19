package site.valron.adab

import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import android.widget.TextView
import site.valron.adab.databinding.ActivityMainBinding

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding

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

        // Example of a call to a native method
        binding.sampleText.text = stringFromJNI()
    }

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
