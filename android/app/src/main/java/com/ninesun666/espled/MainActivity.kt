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
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.core.view.isVisible
import androidx.lifecycle.lifecycleScope
import com.ninesun666.espled.ble.LedManager
import com.ninesun666.espled.databinding.ActivityMainBinding
import com.ninesun666.espled.ota.OtaService
import com.ninesun666.espled.ota.OtaState
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import org.json.JSONArray
import org.json.JSONObject

class MainActivity : AppCompatActivity(), LedManager.Listener {

    /** 扫描结果的一行，下拉框显示顺序与这里一致。 */
    private data class WifiNetwork(val ssid: String, val rssi: Int, val auth: String)

    private lateinit var binding: ActivityMainBinding
    private var adapter: BluetoothAdapter? = null
    private val manager by lazy { LedManager(this) }
    private var scanning = false
    private var ready = false
    private var pollJob: Job? = null
    private var wifiPollJob: Job? = null
    private var wifiNetworks: List<WifiNetwork> = emptyList()

    /** 板子扫描期间为 false，onWifiConfig 拿到 done/error 就置回 true，轮询协程据此收工。 */
    @Volatile
    private var scanSettled = true

    /** 板子当前固件版本（/api/status 的 fw 字段）；旧固件没有该字段，保持 null。 */
    private var boardFw: String? = null

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
        binding.btnOtaCheck.setOnClickListener { checkFirmwareUpdate() }
        binding.btnOtaPush.setOnClickListener { pushFirmware() }

        binding.btnConnect.setOnClickListener {
            if (OtaService.isActive()) {
                // 推送期间板子的唯一连接归前台服务，这里抢连接会互相拆台。
                toast(R.string.ota_in_progress)
                return@setOnClickListener
            }
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
        binding.btnWifiScan.setOnClickListener { startWifiScan() }
        binding.btnWifiApply.setOnClickListener { applyWifiConfig() }
        binding.btnWifiForget.setOnClickListener { forgetWifiNetwork() }
        binding.menuWifiNetwork.setOnItemClickListener { _, _, position, _ ->
            wifiNetworks.getOrNull(position)?.let { binding.etWifiSsid.setText(it.ssid) }
        }
        updatePreview()
        setControlsEnabled(false)
        binding.cardWifi.isVisible = false

        AutoUpdater.onLaunch(this)
        handleInstallIntent(intent)
        // 旋转/进程重建后卡片会回到初始文案；已有更新缓存就直接恢复就绪态（不发网络、不重复请求权限）。
        UpdateChecker.cachedNewerApk(this)?.let {
            showUpdateStatus(getString(R.string.update_ready_fmt, it.version))
            binding.btnCheckUpdate.text = getString(R.string.update_btn_install_fmt, it.version)
        }
        // 固件推送同理：companion 里的已下载文件 + 状态流在重建后恢复 OTA 卡片。
        lifecycleScope.launch {
            OtaService.state.collect { renderOtaState(it) }
        }
        OtaService.downloaded?.let {
            binding.otaStatus.text = getString(R.string.ota_ready_fmt, it.first)
        }
        updateOtaCard()
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

    /* ---- Wi-Fi 配网 -------------------------------------------------------
     * 三个动作都走同一条特征值（0008），命令与 POST /api/wifi 完全一致，
     * 见 docs/wifi_provisioning.md。扫描是板子侧的异步操作：写 {"cmd":"scan"}
     * 只是发起，结果靠反复读同一条特征值拿，所以这里要轮询而不是等回调。 */

    private fun startWifiScan() {
        binding.btnWifiScan.isEnabled = false
        binding.wifiResult.setText(R.string.wifi_scanning)
        manager.writeWifi("""{"cmd":"scan"}""") { ok ->
            runOnUiThread {
                if (!ok) {
                    binding.btnWifiScan.isEnabled = true
                    binding.wifiResult.setText(R.string.wifi_scan_failed)
                    return@runOnUiThread
                }
                pollWifiConfig()
            }
        }
    }

    /** 每 1.5 s 读一次配置，直到扫描落到 done/error，最多 15 s。 */
    private fun pollWifiConfig() {
        wifiPollJob?.cancel()
        scanSettled = false
        wifiPollJob = lifecycleScope.launch {
            var waited = 0
            while (!scanSettled && waited < WIFI_SCAN_TIMEOUT_MS) {
                delay(1500)
                waited += 1500
                manager.readWifiConfig()
            }
            runOnUiThread {
                binding.btnWifiScan.isEnabled = ready
                if (!scanSettled) binding.wifiResult.setText(R.string.wifi_scan_timeout)
            }
        }
    }

    private fun applyWifiConfig() {
        val ssid = binding.etWifiSsid.text?.toString().orEmpty().trim()
        if (ssid.isEmpty()) {
            binding.wifiResult.setText(R.string.wifi_missing_ssid)
            return
        }
        val pass = binding.etWifiPass.text?.toString().orEmpty()
        val json = JSONObject()
            .put("cmd", "set")
            .put("ssid", ssid)
            .put("password", pass)
            .toString()
        manager.writeWifi(json) { ok ->
            runOnUiThread {
                binding.wifiResult.setText(
                    if (ok) R.string.wifi_applied else R.string.wifi_write_failed)
                if (ok) {
                    // 密码交给板子就立刻从输入框清掉：转屏时系统会保存 EditText
                    // 的内容，别让凭据留在内存里多活一轮。
                    binding.etWifiPass.setText("")
                    manager.readWifiConfig()
                    manager.readStatus()
                }
            }
        }
    }

    private fun forgetWifiNetwork() {
        manager.writeWifi("""{"cmd":"forget"}""") { ok ->
            runOnUiThread {
                binding.wifiResult.setText(
                    if (ok) R.string.wifi_forgotten else R.string.wifi_write_failed)
                if (ok) manager.readWifiConfig()
            }
        }
    }

    override fun onWifiConfig(json: String) {
        runOnUiThread { renderWifiConfig(json) }
    }

    private fun renderWifiConfig(json: String) {
        val o = try {
            JSONObject(json)
        } catch (e: Exception) {
            binding.wifiCurrent.text = json
            return
        }

        val ssid = o.optString("ssid")
        binding.wifiCurrent.text = if (ssid.isEmpty()) {
            getString(R.string.wifi_current_none)
        } else {
            getString(R.string.wifi_current_fmt, ssid,
                      wifiSourceLabel(o.optString("source")), o.optString("state"))
        }

        when (o.optString("scan")) {
            "running" -> Unit
            "done" -> {
                scanSettled = true
                val nets = o.optJSONArray("networks") ?: JSONArray()
                wifiNetworks = buildList {
                    for (i in 0 until nets.length()) {
                        val n = nets.optJSONObject(i) ?: continue
                        add(WifiNetwork(n.optString("ssid"), n.optInt("rssi"),
                                        n.optString("auth")))
                    }
                }
                val labels = wifiNetworks.map {
                    getString(R.string.wifi_scan_item_fmt, it.ssid, it.rssi, it.auth)
                }.toTypedArray()
                // 运行时这个 view 是 Material 自动替换出来的 MaterialAutoCompleteTextView
                // （点输入框直接展开全部候选），但 ViewBinding 的静态类型是
                // AutoCompleteTextView，所以只用它的 setAdapter，不碰 setSimpleItems。
                binding.menuWifiNetwork.setAdapter(
                    ArrayAdapter(this, android.R.layout.simple_list_item_1, labels)
                )
                if (wifiNetworks.isEmpty()) binding.wifiResult.setText(R.string.wifi_scan_empty)
                binding.btnWifiScan.isEnabled = ready
            }
            "error" -> {
                scanSettled = true
                binding.wifiResult.text =
                    getString(R.string.wifi_scan_failed_fmt, o.optString("scan_error"))
                binding.btnWifiScan.isEnabled = ready
            }
            else -> scanSettled = true
        }
    }

    private fun wifiSourceLabel(source: String) = when (source) {
        "stored" -> getString(R.string.wifi_source_stored)
        "build" -> getString(R.string.wifi_source_build)
        else -> source
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

    /* ---------- 固件 OTA ---------- */

    /** 检查 Release 里的 wifi_led.bin 并下载缓存；版本是否比板子新留给用户判断。 */
    private fun checkFirmwareUpdate() {
        binding.btnOtaCheck.isEnabled = false
        binding.otaStatus.setText(R.string.ota_checking)
        lifecycleScope.launch {
            when (val r = UpdateChecker.checkFirmware()) {
                is UpdateResult.Available -> {
                    try {
                        val file = UpdateChecker.cachedFirmwareFor(this@MainActivity, r.version)
                            ?: UpdateChecker.downloadFirmware(this@MainActivity, r.version, r.url) { p ->
                                runOnUiThread {
                                    binding.otaStatus.text =
                                        getString(R.string.ota_downloading_fmt, p)
                                }
                            }
                        OtaService.downloaded = r.version to file
                        binding.otaStatus.text = getString(R.string.ota_ready_fmt, r.version)
                    } catch (e: Exception) {
                        binding.otaStatus.text =
                            getString(R.string.ota_download_failed_fmt, e.message ?: "未知错误")
                    }
                }
                is UpdateResult.Failed -> binding.otaStatus.text = r.message
                is UpdateResult.UpToDate -> Unit // checkFirmware 不比较版本，不会走到
            }
            updateOtaCard()
            binding.btnOtaCheck.isEnabled = true
        }
    }

    /** 推送：先拆 MainActivity 的 GATT 链路（板子 NimBLE 单连接），再交前台服务接管。 */
    private fun pushFirmware() {
        val cached = OtaService.downloaded
        if (cached == null || !cached.second.exists()) {
            OtaService.downloaded = null
            binding.otaStatus.setText(R.string.ota_file_gone)
            updateOtaCard()
            return
        }
        val address = manager.bluetoothDevice?.address
        if (!ready || address == null) {
            toast(R.string.ota_need_connection)
            return
        }
        manager.disconnect().enqueue()
        lifecycleScope.launch {
            // 给板子/协议栈一点时间释放连接，3 s 后无论如何都让服务自己重试。
            var waited = 0
            while (manager.isConnected && waited < 3000) {
                delay(100)
                waited += 100
            }
            ContextCompat.startForegroundService(
                this@MainActivity,
                Intent(this@MainActivity, OtaService::class.java)
                    .putExtra(OtaService.EXTRA_ADDRESS, address)
                    .putExtra(OtaService.EXTRA_FILE, cached.second.absolutePath)
                    .putExtra(OtaService.EXTRA_VERSION, cached.first))
        }
    }

    /** OTA 卡片的两行版本 + 推送按钮可用性（连接中、有固件、没在推送）。 */
    private fun updateOtaCard() {
        binding.otaBoardVersion.text = getString(R.string.ota_board_fw_fmt,
            boardFw ?: getString(R.string.ota_fw_unknown))
        binding.otaLatestVersion.text = getString(R.string.ota_latest_fw_fmt,
            OtaService.downloaded?.first ?: getString(R.string.ota_fw_none))
        binding.btnOtaPush.isEnabled =
            ready && OtaService.downloaded != null && !OtaService.isActive()
    }

    /** 镜像 OtaService.state（含旋转后的当前值重放）。 */
    private fun renderOtaState(s: OtaState) {
        when (s) {
            is OtaState.Idle -> Unit
            is OtaState.Connecting -> {
                binding.otaStatus.setText(R.string.ota_state_connecting)
                binding.otaProgress.isIndeterminate = true
            }
            is OtaState.Transferring -> {
                binding.otaStatus.text =
                    getString(R.string.ota_progress_fmt, s.written / 1024, s.total / 1024)
                binding.otaProgress.isIndeterminate = false
                binding.otaProgress.progress =
                    (s.written * 100 / maxOf(s.total, 1L)).toInt()
            }
            is OtaState.Verifying -> {
                binding.otaStatus.setText(R.string.ota_state_verifying)
                binding.otaProgress.isIndeterminate = true
            }
            is OtaState.Rebooting -> {
                binding.otaStatus.setText(R.string.ota_state_rebooting)
                binding.otaProgress.isIndeterminate = false
                binding.otaProgress.progress = 100
            }
            is OtaState.Success -> {
                binding.otaStatus.text = getString(R.string.ota_success_fmt, s.version)
                binding.otaProgress.isIndeterminate = false
                binding.otaProgress.progress = 100
                boardFw = null // 新版本要等重连后读 fw 确认
                updateOtaCard()
            }
            is OtaState.Failed -> {
                binding.otaStatus.text = getString(R.string.ota_failed_fmt, s.reason)
                binding.otaProgress.isIndeterminate = false
                binding.otaProgress.progress = 0
                updateOtaCard()
            }
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
            binding.btnHidLock, binding.btnHidSend, binding.btnOtaPush,
            binding.btnWifiScan, binding.btnWifiApply, binding.btnWifiForget,
            binding.etWifiSsid, binding.etWifiPass, binding.menuWifiNetwork
        ).forEach { it.isEnabled = enabled }
        binding.hidText.isEnabled = enabled
        binding.cardControls.alpha = if (enabled) 1f else 0.4f
        binding.cardScreen.alpha = if (enabled) 1f else 0.4f
        binding.cardHid.alpha = if (enabled) 1f else 0.4f
        binding.cardWifi.alpha = if (enabled) 1f else 0.4f
        binding.cardOta.alpha = if (enabled) 1f else 0.4f
        // btnOtaPush 还要叠加"已有固件且没在推送"条件，交给 updateOtaCard 收口。
        updateOtaCard()
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
            // 老固件没有 0008：卡片整张藏掉，比放着让按钮全死更清楚。
            binding.cardWifi.isVisible = manager.isWifiAvailable()
            if (!manager.isWifiAvailable()) {
                binding.wifiResult.setText(R.string.wifi_unsupported)
            }
        }
        manager.readLedCtl()
        manager.readStatus()
        if (manager.isWifiAvailable()) manager.readWifiConfig()
        startPolling()
    }

    override fun onLedStatus(json: String) {
        // 旧固件没有 fw 字段：optString 回退空串，保持上次的值不动。
        boardFw = try {
            JSONObject(json).optString("fw").takeIf { it.isNotEmpty() } ?: boardFw
        } catch (e: Exception) {
            boardFw
        }
        runOnUiThread {
            updateOtaCard()
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
        wifiPollJob?.cancel()
        scanSettled = true
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

    private companion object {
        /** 板子一次全信道扫描最长十几秒，超过就当成没等到，让用户重试。 */
        const val WIFI_SCAN_TIMEOUT_MS = 15000
    }
}
