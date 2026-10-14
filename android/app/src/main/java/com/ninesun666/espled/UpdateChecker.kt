package com.ninesun666.espled

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.Settings
import androidx.core.content.FileProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
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

    /** 手动按钮和自动流程可能同时下载，串行化避免写坏同一个 .part 文件。 */
    private val downloadMutex = Mutex()

    /** update/ 目录里一个已下载完整的更新包。 */
    data class Cached(val file: File, val version: String)

    /** update/ 目录里比当前安装版本新的最新缓存包；没有则 null。 */
    fun cachedNewerApk(context: Context): Cached? =
        updateDir(context).listFiles { f -> f.name.endsWith(".apk") }
            ?.mapNotNull { f ->
                val v = Regex("esp32-led-(.+)\\.apk").find(f.name)?.groupValues?.get(1)
                if (v != null) Cached(f, v) else null
            }
            ?.filter { compareVersions(it.version, BuildConfig.VERSION_NAME) > 0 }
            ?.maxByOrNull { compareVersions(it.version, BuildConfig.VERSION_NAME) }

    /** 指定版本是否已有完整缓存包（下载成功后 rename 才会出现该文件名）。 */
    fun cachedApkFor(context: Context, version: String): File? {
        val f = File(updateDir(context), "esp32-led-$version.apk")
        return if (f.exists()) f else null
    }

    /** 已是最新版本时清掉缓存目录（旧缓存包必然不再可用）。 */
    fun clearCache(context: Context) {
        updateDir(context).deleteRecursively()
    }

    private fun updateDir(context: Context) = File(context.cacheDir, "update")

    suspend fun check(): UpdateResult = try {
        val json = fetchLatestRelease() ?: return UpdateResult.Failed("暂无发布版本")
        parseLatest(json)
    } catch (e: Exception) {
        UpdateResult.Failed("网络错误：${e.message ?: e.javaClass.simpleName}")
    }

    /** 最新 Release 里的固件附件（wifi_led.bin）。版本比较留给 UI：板子固件版本可能未知。 */
    suspend fun checkFirmware(): UpdateResult = try {
        val json = fetchLatestRelease() ?: return UpdateResult.Failed("暂无发布版本")
        val tag = json.getString("tag_name").removePrefix("v")
        val assets = json.optJSONArray("assets")
        var url: String? = null
        if (assets != null) {
            for (i in 0 until assets.length()) {
                val a = assets.getJSONObject(i)
                if (a.optString("name") == "wifi_led.bin") {
                    url = a.optString("browser_download_url").takeIf { it.isNotEmpty() }
                }
            }
        }
        if (url != null) UpdateResult.Available(tag, url)
        else UpdateResult.Failed("新版本 $tag 没有固件附件 wifi_led.bin")
    } catch (e: Exception) {
        UpdateResult.Failed("网络错误：${e.message ?: e.javaClass.simpleName}")
    }

    /** 抓最新 Release 的 JSON：200 返回解析结果，404 返回 null，其他状态抛错。 */
    private suspend fun fetchLatestRelease(): JSONObject? = withContext(Dispatchers.IO) {
        val base = BuildConfig.UPDATE_API_BASE.trimEnd('/')
        val conn = URL("$base/repos/${BuildConfig.UPDATE_REPO}/releases/latest")
            .openConnection() as HttpURLConnection
        try {
            conn.setRequestProperty("User-Agent", "esp32-led-android")
            conn.setRequestProperty("Accept", "application/vnd.github+json")
            conn.connectTimeout = 10_000
            conn.readTimeout = 15_000
            when (conn.responseCode) {
                404 -> null
                200 -> JSONObject(conn.inputStream.bufferedReader().use { it.readText() })
                else -> throw RuntimeException("HTTP ${conn.responseCode}")
            }
        } finally {
            conn.disconnect()
        }
    }

    private fun parseLatest(json: JSONObject): UpdateResult {
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
    internal fun compareVersions(a: String, b: String): Int {
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

    /** 下载到 esp32-led-{version}.apk：先写 .part，成功后 rename，文件存在即代表该版本完整。 */
    suspend fun download(context: Context, version: String, url: String,
                         onProgress: (Int) -> Unit): File =
        downloadToFile(updateDir(context), "esp32-led-$version.apk", url, onProgress)

    /** 下载到 firmware/firmware-{version}.bin，同一套 .part→rename 约定。 */
    suspend fun downloadFirmware(context: Context, version: String, url: String,
                                 onProgress: (Int) -> Unit): File =
        downloadToFile(firmwareDir(context), "firmware-$version.bin", url, onProgress)

    fun cachedFirmwareFor(context: Context, version: String): File? {
        val f = File(firmwareDir(context), "firmware-$version.bin")
        return if (f.exists()) f else null
    }

    private fun firmwareDir(context: Context) = File(context.cacheDir, "firmware")

    /** 流式 sha256：固件 begin 命令带上它，板子收完先校验再翻转 boot 槽。 */
    fun sha256(file: File): String = file.inputStream().use { input ->
        val md = java.security.MessageDigest.getInstance("SHA-256")
        val buf = ByteArray(64 * 1024)
        while (true) {
            val n = input.read(buf)
            if (n <= 0) break
            md.update(buf, 0, n)
        }
        md.digest().joinToString("") { "%02x".format(it) }
    }

    /** 下载到 {dir}/{baseName}：先写 .part，成功后 rename，文件存在即代表完整。 */
    private suspend fun downloadToFile(dir: File, baseName: String, url: String,
                                       onProgress: (Int) -> Unit): File = downloadMutex.withLock {
        withContext(Dispatchers.IO) {
            dir.mkdirs()
            val part = File(dir, "$baseName.part")
            val conn = URL(url).openConnection() as HttpURLConnection
            try {
                conn.setRequestProperty("User-Agent", "esp32-led-android")
                conn.connectTimeout = 10_000
                conn.readTimeout = 30_000
                if (conn.responseCode != 200) throw RuntimeException("HTTP ${conn.responseCode}")
                val total = conn.contentLengthLong
                conn.inputStream.use { input ->
                    part.outputStream().use { out ->
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
            } catch (e: Exception) {
                part.delete()
                throw e
            } finally {
                conn.disconnect()
            }
            val target = File(dir, baseName)
            target.delete()
            if (!part.renameTo(target)) {
                part.delete()
                throw RuntimeException("重命名缓存文件失败")
            }
            target
        }
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
