package com.ninesun666.espled.ota

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import com.ninesun666.espled.MainActivity
import com.ninesun666.espled.R
import com.ninesun666.espled.UpdateChecker
import com.ninesun666.espled.ble.OtaManager
import java.io.File
import java.io.RandomAccessFile
import kotlin.coroutines.coroutineContext
import kotlin.math.min
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeout
import no.nordicsemi.android.ble.exception.DeviceDisconnectedException
import no.nordicsemi.android.ble.exception.RequestFailedException
// 扩展函数本名是硬关键字 suspend，必须反引号导入+调用（别名导入会让重载解析落到返回 Unit 的档）。
import no.nordicsemi.android.ble.ktx.`suspend`
import org.json.JSONObject

/** 推送状态机；companion 持有 StateFlow，旋转重建后的 Activity 直接 collect。 */
sealed class OtaState {
    object Idle : OtaState()
    object Connecting : OtaState()
    data class Transferring(val written: Long, val total: Long) : OtaState()
    object Verifying : OtaState()
    object Rebooting : OtaState()
    data class Success(val version: String) : OtaState()
    data class Failed(val reason: String) : OtaState()
}

/**
 * 前台服务（dataSync）：把固件镜像经 BLE 推给板子，锁屏/退出页面都继续。
 *
 * 板子 NimBLE 单连接：MainActivity 先断开自己的 GATT 再启动本服务，服务用
 * 自己的 OtaManager 连上同一设备。协议要点（见固件 ble_gatt/ota_update）：
 * 断开不清 busy 状态，所以每次 begin 前先读 0005，busy 则 abort——App 被
 * 杀掉留下的半截传输就是这样恢复的。
 */
class OtaService : Service() {

    companion object {
        const val EXTRA_ADDRESS = "address"
        const val EXTRA_FILE = "file"
        const val EXTRA_VERSION = "version"

        val state = MutableStateFlow<OtaState>(OtaState.Idle)
        /** 已下载的固件（版本 + 文件）；companion 持有，旋转不丢。 */
        @Volatile
        var downloaded: Pair<String, File>? = null

        private const val CHANNEL_ID = "ota"
        private const val NOTIF_ID = 2
        private const val NOTIF_FINAL_ID = 3

        fun isActive(): Boolean = state.value.let {
            it is OtaState.Connecting || it is OtaState.Transferring ||
                it is OtaState.Verifying || it is OtaState.Rebooting
        }
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var job: Job? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID,
                getString(R.string.ota_channel_name),
                NotificationManager.IMPORTANCE_LOW))
        // 通知镜像状态流；进度节流由发送侧（每 16 块）保证。
        scope.launch {
            state.collect { s -> nm.notify(NOTIF_ID, buildNotification(textFor(s), progressFor(s))) }
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // 通知权限被拒也必须调 startForeground（FGS 照常运行，只是抽屉里看不见）。
        ServiceCompat.startForeground(this, NOTIF_ID,
            buildNotification(getString(R.string.ota_state_connecting), 0),
            if (Build.VERSION.SDK_INT >= 29) ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC else 0)
        if (job?.isActive == true) return START_NOT_STICKY // 双击防抖

        val device = intent?.getStringExtra(EXTRA_ADDRESS)?.let {
            (getSystemService(BLUETOOTH_SERVICE) as BluetoothManager)
                .adapter?.getRemoteDevice(it)
        }
        val file = intent?.getStringExtra(EXTRA_FILE)?.let(::File)
        val version = intent?.getStringExtra(EXTRA_VERSION) ?: ""
        if (device == null || file == null || !file.exists() ||
            ContextCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) !=
                PackageManager.PERMISSION_GRANTED
        ) {
            finish(OtaState.Failed(getString(R.string.ota_err_precondition)))
            return START_NOT_STICKY
        }

        job = scope.launch {
            try {
                withTimeout(10 * 60_000L) { runTransfer(device, file, version) }
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                finish(OtaState.Failed(e.message ?: e.javaClass.simpleName))
            }
        }
        return START_NOT_STICKY // 半截传输绝不能被系统重放
    }

    private suspend fun runTransfer(device: BluetoothDevice, file: File, version: String) {
        val manager = OtaManager(this)
        try {
            state.value = OtaState.Connecting
            manager.connect(device)
                .useAutoConnect(false)
                .retry(3, 1000) // MainActivity 的链路可能还没拆完，板子单连接
                .timeout(15_000)
                .`suspend`()

            val mtu = manager.negotiateMtu()
            val chunkSize = min(480, mtu - 3).coerceAtLeast(20)
            val size = file.length()
            val sha = UpdateChecker.sha256(file)
            val buf = ByteArray(chunkSize)

            for (attempt in 0..1) { // 仅一次自动重试
                if (attempt == 0) {
                    ensureNotBusy(manager) // App 被杀留下的陈旧传输
                } else {
                    // busy 时 begin 直接 0x83：abort 是重试的必经步骤
                    runCatching { manager.writeCtl("""{"cmd":"abort"}""") }
                    if (!manager.isConnected) {
                        manager.connect(device).useAutoConnect(false)
                            .retry(3, 1000).timeout(15_000).`suspend`()
                        manager.negotiateMtu()
                        ensureNotBusy(manager)
                    }
                }
                try {
                    manager.writeCtl("""{"cmd":"begin","size":$size,"sha256":"$sha"}""")
                    state.value = OtaState.Transferring(0, size)
                    var written = 0L
                    var nth = 0
                    RandomAccessFile(file, "r").use { raf ->
                        while (true) {
                            coroutineContext.ensureActive()
                            val len = raf.read(buf)
                            if (len <= 0) break
                            manager.writeChunk(if (len == chunkSize) buf else buf.copyOf(len))
                            written += len
                            if (++nth % 16 == 0) state.value = OtaState.Transferring(written, size)
                        }
                    }
                    state.value = OtaState.Transferring(size, size)
                    state.value = OtaState.Verifying
                    manager.writeCtl("""{"cmd":"end"}""") // 0x83 = sha/flash 校验失败
                    break
                } catch (e: DeviceDisconnectedException) {
                    if (attempt == 1) throw e // 下一轮循环顶部重连
                } catch (e: RequestFailedException) {
                    if (attempt == 1) throw e // 下一轮循环顶部 abort 重来
                }
            }

            state.value = OtaState.Rebooting // end 接受后板子约 1 s 重启
            delay(6_000)
            finish(OtaState.Success(version))
        } finally {
            runCatching { if (manager.isConnected) manager.disconnect().enqueue() }
        }
    }

    /** 断开不清 busy（固件只在断开时恢复广播），begin 前必须确认并清掉。 */
    private suspend fun ensureNotBusy(manager: OtaManager) {
        if (JSONObject(manager.readCtl()).optBoolean("busy", false)) {
            runCatching { manager.writeCtl("""{"cmd":"abort"}""") }
        }
    }

    private fun finish(s: OtaState) {
        state.value = s
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.notify(NOTIF_FINAL_ID, buildNotification(textFor(s), null, final = true))
        ServiceCompat.stopForeground(this, ServiceCompat.STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    override fun onDestroy() {
        scope.cancel() // 取消在途写（suspend 可取消），连接断开由系统收尾
    }

    private fun textFor(s: OtaState): String = when (s) {
        is OtaState.Idle -> getString(R.string.ota_state_connecting)
        is OtaState.Connecting -> getString(R.string.ota_state_connecting)
        is OtaState.Transferring -> getString(R.string.ota_notif_transferring_fmt,
            (s.written * 100 / s.total.coerceAtLeast(1)).toInt())
        is OtaState.Verifying -> getString(R.string.ota_state_verifying)
        is OtaState.Rebooting -> getString(R.string.ota_state_rebooting)
        is OtaState.Success -> getString(R.string.ota_success_fmt, s.version)
        is OtaState.Failed -> getString(R.string.ota_failed_fmt, s.reason)
    }

    private fun progressFor(s: OtaState): Int? = when (s) {
        is OtaState.Transferring ->
            (s.written * 100 / s.total.coerceAtLeast(1)).toInt()
        else -> null
    }

    private fun buildNotification(text: String, progress: Int?, final: Boolean = false) =
        NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_launcher)
            .setContentTitle(getString(R.string.ota_channel_name))
            .setContentText(text)
            .setContentIntent(PendingIntent.getActivity(
                this, 0,
                Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
                PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT))
            .setOnlyAlertOnce(true)
            .setProgress(100, progress ?: 0, progress == null && !final)
            .setAutoCancel(final)
            .build()
}
