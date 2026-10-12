package com.ninesun666.espled

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.core.content.FileProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

sealed class UpdateResult {
    object UpToDate : UpdateResult()
    data class Available(val version: String, val url: String) : UpdateResult()
    data class Failed(val message: String) : UpdateResult()
}

object UpdateChecker {

    suspend fun check(): UpdateResult = withContext(Dispatchers.IO) {
        try {
            val base = BuildConfig.UPDATE_API_BASE.trimEnd('/')
            val conn = URL("$base/repos/${BuildConfig.UPDATE_REPO}/releases/latest")
                .openConnection() as HttpURLConnection
            try {
                conn.setRequestProperty("User-Agent", "esp32-led-android")
                conn.setRequestProperty("Accept", "application/vnd.github+json")
                conn.connectTimeout = 10_000
                conn.readTimeout = 15_000
                when (conn.responseCode) {
                    404 -> UpdateResult.Failed("暂无发布版本")
                    200 -> parseLatest(conn.inputStream.bufferedReader().use { it.readText() })
                    else -> UpdateResult.Failed("检查失败：HTTP ${conn.responseCode}")
                }
            } finally {
                conn.disconnect()
            }
        } catch (e: Exception) {
            UpdateResult.Failed("网络错误：${e.message ?: e.javaClass.simpleName}")
        }
    }

    private fun parseLatest(body: String): UpdateResult {
        val json = JSONObject(body)
        val tag = json.getString("tag_name").removePrefix("v")
        if (compareVersions(tag, BuildConfig.VERSION_NAME) <= 0) return UpdateResult.UpToDate
        val assets = json.optJSONArray("assets")
        if (assets != null) {
            for (i in 0 until assets.length()) {
                val a = assets.getJSONObject(i)
                if (a.optString("name").endsWith(".apk", ignoreCase = true)) {
                    val url = a.optString("browser_download_url")
                    if (url.isNotEmpty()) return UpdateResult.Available(tag, url)
                }
            }
        }
        return UpdateResult.Failed("新版本 $tag 没有 APK 附件")
    }

    /** 数字逐段比较：a 新于 b 返回正数。非数字段（-beta 之类）忽略。 */
    private fun compareVersions(a: String, b: String): Int {
        val pa = versionNumbers(a)
        val pb = versionNumbers(b)
        for (i in 0 until maxOf(pa.size, pb.size)) {
            val d = pa.getOrElse(i) { 0 } - pb.getOrElse(i) { 0 }
            if (d != 0) return d
        }
        return 0
    }

    private fun versionNumbers(v: String) = v.split('.', '-', '_').mapNotNull { seg ->
        seg.takeWhile(Char::isDigit).takeIf { it.isNotEmpty() }?.toIntOrNull()
    }

    suspend fun download(context: Context, url: String, onProgress: (Int) -> Unit): File =
        withContext(Dispatchers.IO) {
            val dir = File(context.cacheDir, "update").apply { mkdirs() }
            val apk = File(dir, "esp32-led.apk")
            val conn = URL(url).openConnection() as HttpURLConnection
            try {
                conn.setRequestProperty("User-Agent", "esp32-led-android")
                conn.connectTimeout = 10_000
                conn.readTimeout = 30_000
                if (conn.responseCode != 200) throw RuntimeException("HTTP ${conn.responseCode}")
                val total = conn.contentLengthLong
                conn.inputStream.use { input ->
                    apk.outputStream().use { out ->
                        val buf = ByteArray(64 * 1024)
                        var done = 0L
                        while (true) {
                            val n = input.read(buf)
                            if (n <= 0) break
                            out.write(buf, 0, n)
                            done += n
                            if (total > 0) onProgress((done * 100 / total).toInt())
                        }
                    }
                }
            } finally {
                conn.disconnect()
            }
            apk
        }

    /**
     * 拉起系统安装界面。API 26+ 需要"安装未知应用"授权，
     * 未授权时跳到系统设置页，返回 false 让调用方提示用户再试。
     */
    fun install(context: Context, apk: File): Boolean {
        if (!context.packageManager.canRequestPackageInstalls()) {
            context.startActivity(
                Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                    Uri.parse("package:${context.packageName}"))
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            )
            return false
        }
        val uri = FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", apk)
        context.startActivity(
            Intent(Intent.ACTION_VIEW)
                .setDataAndType(uri, "application/vnd.android.package-archive")
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
        )
        return true
    }
}
