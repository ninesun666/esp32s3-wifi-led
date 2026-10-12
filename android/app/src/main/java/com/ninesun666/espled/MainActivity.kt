package com.ninesun666.espled

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.ParcelUuid
import android.view.View
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import com.ninesun666.espled.ble.LedManager
import com.ninesun666.espled.databinding.ActivityMainBinding
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject

class MainActivity : AppCompatActivity(), LedManager.Listener {

    private lateinit var binding: ActivityMainBinding
    private var adapter: BluetoothAdapter? = null
    private val manager by lazy { LedManager(this) }
    private var scanning = false
    private var ready = false
    private var pollJob: Job? = null

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { grants ->
        if (grants.values.all { it }) startScan() else toast(R.string.toast_no_permission)
    }

    /** 通知权限单独请求，不并进蓝牙那一组——那边是全 granted 才开始扫描。 */
    private val notifPermLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted -> if (granted) AutoUpdater.notifyIfReady(applicationContext) }

    @SuppressLint("MissingPermission")
    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            if (!scanning) return
            stopScanQuietly()
            toast(R.string.toast_found)
            manager.connect(result.device).retry(3, 100).useAutoConnect(false).enqueue()
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            toast(R.string.toast_scan_failed)
            setButtonState()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)
        adapter = (getSystemService(BLUETOOTH_SERVICE) as BluetoothManager).adapter
        manager.listener = this

        binding.updateVersion.text = getString(R.string.update_version_fmt, BuildConfig.VERSION_NAME)
        binding.btnCheckUpdate.setOnClickListener { checkForUpdate() }

        binding.btnConnect.setOnClickListener {
            if (manager.isConnected) manager.disconnect().enqueue() else requestAndScan()
        }
        binding.sldR.addOnChangeListener { _, _, _ -> updatePreview() }
        binding.sldG.addOnChangeListener { _, _, _ -> updatePreview() }
        binding.sldB.addOnChangeListener { _, _, _ -> updatePreview() }
        binding.sldBri.addOnChangeListener { _, value, _ ->
            binding.briValue.text = "${value.toInt()}%"
        }
        binding.btnSolid.setOnClickListener { sendCtl("solid") }
        binding.btnBreathe.setOnClickListener { sendCtl("breathe") }
        binding.btnRainbow.setOnClickListener { sendCtl("rainbow") }
        binding.btnOff.setOnClickListener { sendCtl("off") }
        binding.btnAuto.setOnClickListener {
            manager.writeLed("""{"manual":false}""") { ok -> onWriteDone(ok) }
        }
        binding.btnRefresh.setOnClickListener {
            manager.readStatus()
            manager.readLedCtl()
        }
        binding.btnEmoteHappy.setOnClickListener { sendScreen("""{"emote":"happy"}""") }
        binding.btnEmoteLaugh.setOnClickListener { sendScreen("""{"emote":"laughing"}""") }
        binding.btnEmoteWink.setOnClickListener { sendScreen("""{"emote":"winking"}""") }
        binding.btnEmoteSurprised.setOnClickListener { sendScreen("""{"emote":"surprised"}""") }
        binding.btnEmoteSad.setOnClickListener { sendScreen("""{"emote":"sad"}""") }
        binding.btnEmoteCry.setOnClickListener { sendScreen("""{"emote":"crying"}""") }
        binding.btnEmoteSleep.setOnClickListener { sendScreen("""{"emote":"sleepy"}""") }
        binding.btnBars.setOnClickListener { sendScreen("""{"pattern":"bars"}""") }
        binding.btnFill.setOnClickListener {
            val o = JSONObject()
                .put("r", binding.sldR.value.toInt())
                .put("g", binding.sldG.value.toInt())
                .put("b", binding.sldB.value.toInt())
            sendScreen(o.toString())
        }
        binding.btnHidPlay.setOnClickListener { sendHid("""{"type":"media","key":"play"}""") }
        binding.btnHidPrev.setOnClickListener { sendHid("""{"type":"media","key":"prev"}""") }
        binding.btnHidNext.setOnClickListener { sendHid("""{"type":"media","key":"next"}""") }
        binding.btnHidMute.setOnClickListener { sendHid("""{"type":"media","key":"mute"}""") }
        binding.btnHidVolUp.setOnClickListener { sendHid("""{"type":"media","key":"volup"}""") }
        binding.btnHidVolDown.setOnClickListener { sendHid("""{"type":"media","key":"voldown"}""") }
        binding.btnHidLock.setOnClickListener { sendHid("""{"type":"media","key":"lock"}""") }
        binding.btnHidSend.setOnClickListener {
            val text = binding.hidText.text?.toString().orEmpty().trim()
            if (text.isNotEmpty()) sendHid(JSONObject().put("type", "text").put("text", text).toString())
        }
        updatePreview()
        setControlsEnabled(false)

        AutoUpdater.onLaunch(this)
        handleInstallIntent(intent)
        // 旋转/进程重建后卡片会回到初始文案；已有更新缓存就直接恢复就绪态（不发网络、不重复请求权限）。
        UpdateChecker.cachedNewerApk(this)?.let {
            showUpdateStatus(getString(R.string.update_ready_fmt, it.version))
            binding.btnCheckUpdate.text = getString(R.string.update_btn_install_fmt, it.version)
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handleInstallIntent(intent)
    }

    /** 通知点按进来：缓存还在就直接装；被系统清了就回落到检查更新重下。 */
    private fun handleInstallIntent(intent: Intent) {
        if (!intent.getBooleanExtra(AutoUpdater.EXTRA_INSTALL, false)) return
        val cached = UpdateChecker.cachedNewerApk(this) ?: return checkForUpdate()
        showUpdateStatus(getString(R.string.update_ready_fmt, cached.version))
        if (!UpdateChecker.install(this, cached.file)) {
            binding.updateStatus.setText(R.string.update_grant_hint)
        }
    }

    /** 供 AutoUpdater 刷更新状态（进度回调在 IO 线程，需经 runOnUiThread）。 */
    fun showUpdateStatus(text: String) {
        binding.updateStatus.text = text
    }

    /** 自动下载完成（前台时）回调：卡片就绪态 + 按钮变安装 + 发通知。 */
    fun onUpdateReady(version: String) {
        showUpdateStatus(getString(R.string.update_ready_fmt, version))
        binding.btnCheckUpdate.text = getString(R.string.update_btn_install_fmt, version)
        requestNotifyPermissionThenPost()
    }

    private fun requestNotifyPermissionThenPost() {
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(this,
                android.Manifest.permission.POST_NOTIFICATIONS) !=
                PackageManager.PERMISSION_GRANTED
        ) {
            notifPermLauncher.launch(android.Manifest.permission.POST_NOTIFICATIONS)
        } else {
            AutoUpdater.notifyIfReady(applicationContext)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        AutoUpdater.host = null
        if (manager.isConnected) manager.disconnect().enqueue()
    }

    private fun sendCtl(fx: String) {
        val o = JSONObject()
            .put("manual", true)
            .put("fx", fx)
            .put("rgb", JSONArray(listOf(
                binding.sldR.value.toInt(),
                binding.sldG.value.toInt(),
                binding.sldB.value.toInt())))
            .put("brightness", binding.sldBri.value.toInt())
        manager.writeLed(o.toString()) { ok -> onWriteDone(ok) }
    }

    private fun onWriteDone(ok: Boolean) {
        if (ok) manager.readLedCtl() else toast(R.string.toast_write_failed)
    }

    private fun sendScreen(json: String) {
        manager.writeScreen(json) { ok -> if (!ok) toast(R.string.toast_write_failed) }
    }

    private fun sendHid(json: String) {
        manager.writeHid(json) { ok ->
            if (!ok) toast(if (manager.isHidAvailable()) R.string.toast_write_failed
                           else R.string.hid_unavailable)
        }
    }

    private fun checkForUpdate() {
        // 已有完整缓存包就直接装，不走网络（自动流程下载完成后的按钮路径）。
        UpdateChecker.cachedNewerApk(this)?.let { c ->
            onUpdateReady(c.version)
            if (!UpdateChecker.install(this, c.file)) {
                binding.updateStatus.setText(R.string.update_grant_hint)
            }
            return
        }
        binding.btnCheckUpdate.isEnabled = false
        binding.updateStatus.setText(R.string.update_checking)
        lifecycleScope.launch {
            when (val r = UpdateChecker.check()) {
                is UpdateResult.UpToDate -> binding.updateStatus.setText(R.string.update_latest)
                is UpdateResult.Failed -> binding.updateStatus.text = r.message
                is UpdateResult.Available -> {
                    binding.updateStatus.text = getString(R.string.update_found_fmt, r.version, 0)
                    try {
                        val apk = UpdateChecker.download(this@MainActivity, r.version, r.url) { p ->
                            runOnUiThread {
                                binding.updateStatus.text =
                                    getString(R.string.update_found_fmt, r.version, p)
                            }
                        }
                        binding.updateStatus.setText(R.string.update_downloaded)
                        if (!UpdateChecker.install(this@MainActivity, apk)) {
                            binding.updateStatus.setText(R.string.update_grant_hint)
                        }
                    } catch (e: Exception) {
                        binding.updateStatus.text =
                            getString(R.string.update_download_failed_fmt, e.message ?: "未知错误")
                    }
                }
            }
            binding.btnCheckUpdate.isEnabled = true
        }
    }

    private fun updatePreview() {
        binding.colorPreview.setBackgroundColor(
            Color.rgb(
                binding.sldR.value.toInt(),
                binding.sldG.value.toInt(),
                binding.sldB.value.toInt()
            )
        )
    }

    private fun requestAndScan() {
        val needed = if (Build.VERSION.SDK_INT >= 31)
            arrayOf(android.Manifest.permission.BLUETOOTH_SCAN,
                android.Manifest.permission.BLUETOOTH_CONNECT)
        else
            arrayOf(android.Manifest.permission.ACCESS_FINE_LOCATION)
        val missing = needed.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) startScan() else permissionLauncher.launch(missing.toTypedArray())
    }

    @SuppressLint("MissingPermission")
    private fun startScan() {
        val a = adapter
        if (a == null || !a.isEnabled) {
            toast(R.string.toast_bt_off)
            return
        }
        val scanner = a.bluetoothLeScanner
        if (scanner == null) {
            toast(R.string.toast_scan_failed)
            return
        }
        scanning = true
        setButtonState()
        scanner.startScan(
            listOf(
                ScanFilter.Builder()
                    .setServiceUuid(ParcelUuid(LedManager.SERVICE_UUID))
                    .build()
            ),
            ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_BALANCED)
                .build(),
            scanCallback
        )
        lifecycleScope.launch {
            delay(12000)
            if (scanning) {
                stopScanQuietly()
                toast(R.string.toast_not_found)
                setButtonState()
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun stopScanQuietly() {
        scanning = false
        runCatching { adapter?.bluetoothLeScanner?.stopScan(scanCallback) }
    }

    private fun setButtonState() {
        binding.btnConnect.setText(when {
            ready -> R.string.btn_disconnect
            scanning -> R.string.btn_scanning
            manager.isConnected -> R.string.btn_connecting
            else -> R.string.btn_connect
        })
    }

    private fun setControlsEnabled(enabled: Boolean) {
        listOf(
            binding.sldR, binding.sldG, binding.sldB, binding.sldBri,
            binding.btnSolid, binding.btnBreathe, binding.btnRainbow, binding.btnOff,
            binding.btnAuto, binding.btnRefresh,
            binding.btnEmoteHappy, binding.btnEmoteLaugh, binding.btnEmoteWink,
            binding.btnEmoteSurprised, binding.btnEmoteSad, binding.btnEmoteCry,
            binding.btnEmoteSleep, binding.btnFill, binding.btnBars,
            binding.btnHidPlay, binding.btnHidPrev, binding.btnHidNext,
            binding.btnHidMute, binding.btnHidVolUp, binding.btnHidVolDown,
            binding.btnHidLock, binding.btnHidSend
        ).forEach { it.isEnabled = enabled }
        binding.hidText.isEnabled = enabled
        binding.cardControls.alpha = if (enabled) 1f else 0.4f
        binding.cardScreen.alpha = if (enabled) 1f else 0.4f
        binding.cardHid.alpha = if (enabled) 1f else 0.4f
    }

    private fun startPolling() {
        pollJob?.cancel()
        pollJob = lifecycleScope.launch {
            while (isActive && ready) {
                delay(5000)
                if (!ready) break
                manager.readStatus()
            }
        }
    }

    override fun onLedReady() {
        ready = true
        runOnUiThread {
            setControlsEnabled(true)
            setButtonState()
        }
        manager.readLedCtl()
        manager.readStatus()
        startPolling()
    }

    override fun onLedStatus(json: String) {
        runOnUiThread {
            binding.statusText.text = try {
                JSONObject(json).toString(2)
            } catch (e: Exception) {
                json
            }
        }
    }

    override fun onLedCtl(json: String) {
        runOnUiThread { syncUi(json) }
    }

    override fun onLedDisconnected() {
        ready = false
        pollJob?.cancel()
        runOnUiThread {
            setControlsEnabled(false)
            setButtonState()
            binding.modeLabel.setText(R.string.mode_idle)
            binding.statusText.setText(R.string.status_idle)
        }
    }

    private fun syncUi(json: String) {
        try {
            val o = JSONObject(json)
            if (!o.optBoolean("manual", false)) {
                binding.modeLabel.setText(R.string.mode_auto)
                return
            }
            val fx = o.optString("fx", "solid")
            val rgb = o.optJSONArray("rgb")
            if (rgb != null && rgb.length() == 3) {
                binding.sldR.value = rgb.optInt(0).coerceIn(0, 255).toFloat()
                binding.sldG.value = rgb.optInt(1).coerceIn(0, 255).toFloat()
                binding.sldB.value = rgb.optInt(2).coerceIn(0, 255).toFloat()
            }
            binding.sldBri.value = o.optInt("brightness", 100).coerceIn(0, 100).toFloat()
            binding.modeLabel.text = getString(R.string.mode_manual, fxZh(fx))
        } catch (e: Exception) {
            // Malformed payload: leave the UI as-is.
        }
    }

    private fun fxZh(fx: String) = when (fx) {
        "solid" -> getString(R.string.fx_solid)
        "breathe" -> getString(R.string.fx_breathe)
        "rainbow" -> getString(R.string.fx_rainbow)
        "off" -> getString(R.string.fx_off)
        else -> fx
    }

    private fun toast(res: Int) {
        Toast.makeText(this, res, Toast.LENGTH_SHORT).show()
    }
}
