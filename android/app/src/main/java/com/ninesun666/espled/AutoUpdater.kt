package com.ninesun666.espled

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch

/**
 * 自动更新：启动时静默检查一次，有新版就后台下载，完成后发通知 /
 * 刷新页面卡片，由用户点一下安装（侧载 App 无法静默自装）。
 *
 * 用应用级 scope 而不是 lifecycleScope：返回键退出后下载也要继续，
 * 通知才能到达。host 指向当前 MainActivity（onCreate 注册、onDestroy
 * 置空），用于把进度/结果刷到页面；为空即退化为纯通知。
 */
object AutoUpdater {

    /** 通知点按的 intent extra：让 MainActivity 直接拉起安装。 */
    const val EXTRA_INSTALL = "install_update"
    private const val CHANNEL_ID = "update"
    private const val NOTIF_ID = 1

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var job: Job? = null
    private var ranThisProcess = false

    /** 当前前台宿主；旋转重建后的 onCreate 会重新赋值。 */
    internal var host: MainActivity? = null

    /** MainActivity.onCreate 调：注册宿主，并（每进程一次）静默检查更新。 */
    fun onLaunch(activity: MainActivity) {
        host = activity
        if (ranThisProcess || job?.isActive == true) return
        ranThisProcess = true
        val app = activity.applicationContext
        job = scope.launch {
            when (val r = UpdateChecker.check()) {
                is UpdateResult.UpToDate -> UpdateChecker.clearCache(app)
                is UpdateResult.Failed -> Unit // 自动路径静默；错误信息留给手动按钮
                is UpdateResult.Available -> fetchAndNotify(app, r)
            }
        }
    }

    private suspend fun fetchAndNotify(app: Context, r: UpdateResult.Available) {
        if (UpdateChecker.cachedApkFor(app, r.version) == null) {
            try {
                postStatus(app.getString(R.string.update_found_fmt, r.version, 0))
                UpdateChecker.download(app, r.version, r.url) { p ->
                    postStatus(app.getString(R.string.update_found_fmt, r.version, p))
                }
            } catch (e: Exception) {
                postStatus(app.getString(R.string.update_download_failed_fmt,
                    e.message ?: "未知错误"))
                return
            }
        }
        val h = host
        if (h == null || h.isFinishing || h.isDestroyed) {
            notifyIfReady(app) // 用户已退到后台：能发通知就发
        } else {
            h.onUpdateReady(r.version) // scope 是 Main dispatcher，直接调
        }
    }

    private fun postStatus(text: String) {
        val h = host ?: return
        h.runOnUiThread { h.showUpdateStatus(text) } // onProgress 在 IO 线程回调
    }

    /** 发"新版本已就绪"通知；未授权（API 33+）或没有可装缓存时静默跳过。 */
    fun notifyIfReady(context: Context) {
        if (Build.VERSION.SDK_INT >= 33 &&
            ContextCompat.checkSelfPermission(context, Manifest.permission.POST_NOTIFICATIONS) !=
                PackageManager.PERMISSION_GRANTED
        ) return
        val cached = UpdateChecker.cachedNewerApk(context) ?: return
        val nm = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID,
                context.getString(R.string.update_channel_name),
                NotificationManager.IMPORTANCE_DEFAULT))
        val pi = PendingIntent.getActivity(
            context, 0,
            Intent(context, MainActivity::class.java)
                .putExtra(EXTRA_INSTALL, true)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val n = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_launcher)
            .setContentTitle(context.getString(R.string.update_notif_title_fmt, cached.version))
            .setContentText(context.getString(R.string.update_notif_text))
            .setContentIntent(pi)
            .setAutoCancel(true)
            .build()
        nm.notify(NOTIF_ID, n)
    }
}
