package com.ninesun666.espled.ble

import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.content.Context
import java.util.UUID
import no.nordicsemi.android.ble.BleManager
// 扩展函数本名是硬关键字 suspend，必须反引号导入+调用（别名导入会让重载解析落到返回 Unit 的档）。
import no.nordicsemi.android.ble.ktx.`suspend`

/*
 * GATT profile for a firmware push, mirroring LedManager's structure but
 * discovering only the OTA pair of the same service:
 *
 *   0005 control -- JSON {"cmd":"begin","size","sha256"} / {"cmd":"end"} /
 *                   {"cmd":"abort"} on write, {"busy","written","total"} on read
 *   0006 data    -- raw image chunks, <=512 bytes each
 *
 * All operations are suspend: ble-ktx 2.10 names the extension `suspend()`.
 * The service calls them sequentially, so at most one write is ever queued --
 * ~3200 chunks chain without recursion (plain loop) or queue pressure.
 */
class OtaManager(context: Context) : BleManager(context) {

    private var ctlChar: BluetoothGattCharacteristic? = null
    private var dataChar: BluetoothGattCharacteristic? = null

    override fun isRequiredServiceSupported(gatt: BluetoothGatt): Boolean {
        val service = gatt.getService(LedManager.SERVICE_UUID) ?: return false
        ctlChar = service.getCharacteristic(OTA_CTL_CHAR_UUID)
        dataChar = service.getCharacteristic(OTA_DATA_CHAR_UUID)
        return ctlChar != null && dataChar != null
    }

    override fun initialize() {
        // Empty on purpose: the service negotiates MTU explicitly so it can
        // size the chunks from the value that was actually agreed.
    }

    override fun onServicesInvalidated() {
        ctlChar = null
        dataChar = null
    }

    /** @return the negotiated MTU (firmware prefers 512). */
    suspend fun negotiateMtu(): Int = requestMtu(512).`suspend`()

    /** One control-JSON command (begin/end/abort); app errors surface as exceptions. */
    suspend fun writeCtl(json: String) {
        val characteristic = ctlChar ?: throw IllegalStateException("OTA chars not ready")
        writeCharacteristic(
            characteristic,
            json.toByteArray(Charsets.UTF_8),
            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        ).`suspend`()
    }

    /** @return the progress JSON {"busy":bool,"written":N,"total":N}. */
    suspend fun readCtl(): String {
        val characteristic = ctlChar ?: throw IllegalStateException("OTA chars not ready")
        val data = readCharacteristic(characteristic).`suspend`()
        return String(data.value ?: ByteArray(0), Charsets.UTF_8)
    }

    /** One data chunk, write-with-response: every chunk is acked, so a flash
     * error (0x83) or a stalled board fails this write instead of silently
     * corrupting the stream. 10 s stall guard per chunk. */
    suspend fun writeChunk(chunk: ByteArray) {
        val characteristic = dataChar ?: throw IllegalStateException("OTA chars not ready")
        writeCharacteristic(
            characteristic,
            chunk,
            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        )
            .timeout(WRITE_TIMEOUT_MS)
            .`suspend`()
    }

    companion object {
        private const val WRITE_TIMEOUT_MS = 10_000L

        val OTA_CTL_CHAR_UUID: UUID = UUID.fromString("5d400005-7a9b-4c8e-b2f4-38a1c6e0d951")
        val OTA_DATA_CHAR_UUID: UUID = UUID.fromString("5d400006-7a9b-4c8e-b2f4-38a1c6e0d951")
    }
}
