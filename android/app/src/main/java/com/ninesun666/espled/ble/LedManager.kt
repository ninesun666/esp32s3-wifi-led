package com.ninesun666.espled.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.content.Context
import android.util.Log
import no.nordicsemi.android.ble.BleManager
import no.nordicsemi.android.ble.observer.ConnectionObserver
import java.util.UUID

/*
 * Mirrors the Nordic Blinky sample structure: a BleManager subclass owns the
 * GATT profile, exposes typed operations, and reports readiness through a
 * ConnectionObserver. Protocol is the ESP32-LED profile from the firmware's
 * ble_gatt component: one custom service with a read/write LED characteristic,
 * a read-only status characteristic, write-only screen and HID characteristics,
 * and a read/write Wi-Fi provisioning characteristic — the LED, status and
 * Wi-Fi ones carry the same JSON as the board's HTTP API.
 */
class LedManager(context: Context) : BleManager(context) {

    interface Listener {
        fun onLedReady()
        fun onLedStatus(json: String)
        fun onLedCtl(json: String)
        fun onWifiConfig(json: String)
        fun onLedDisconnected()
    }

    var listener: Listener? = null

    private var ledChar: BluetoothGattCharacteristic? = null
    private var statusChar: BluetoothGattCharacteristic? = null
    private var screenChar: BluetoothGattCharacteristic? = null
    private var wifiChar: BluetoothGattCharacteristic? = null
    private var hidChar: BluetoothGattCharacteristic? = null

    init {
        setConnectionObserver(object : ConnectionObserver {
            override fun onDeviceConnecting(device: BluetoothDevice) {}

            override fun onDeviceConnected(device: BluetoothDevice) {}

            override fun onDeviceFailedToConnect(device: BluetoothDevice, reason: Int) {
                listener?.onLedDisconnected()
            }

            override fun onDeviceReady(device: BluetoothDevice) {
                listener?.onLedReady()
            }

            override fun onDeviceDisconnecting(device: BluetoothDevice) {}

            override fun onDeviceDisconnected(device: BluetoothDevice, reason: Int) {
                listener?.onLedDisconnected()
            }
        })
    }

    override fun isRequiredServiceSupported(gatt: BluetoothGatt): Boolean {
        val service = gatt.getService(SERVICE_UUID) ?: return false
        ledChar = service.getCharacteristic(LED_CHAR_UUID)
        statusChar = service.getCharacteristic(STATUS_CHAR_UUID)
        screenChar = service.getCharacteristic(SCREEN_CHAR_UUID)
        // Optional: only present from the OTA/HID firmware generation onward.
        hidChar = service.getCharacteristic(HID_CHAR_UUID)
        // Optional: Wi-Fi provisioning arrived with the dynamic-credential firmware.
        wifiChar = service.getCharacteristic(WIFI_CHAR_UUID)
        val writable = ledChar?.let {
            (it.properties and BluetoothGattCharacteristic.PROPERTY_WRITE) != 0
        } ?: false
        return writable && statusChar != null
    }

    override fun initialize() {
        // Firmware opens MTU 512 so a whole ctl/status JSON fits in one GATT op.
        requestMtu(512).enqueue()
    }

    override fun onServicesInvalidated() {
        ledChar = null
        statusChar = null
        screenChar = null
        hidChar = null
        wifiChar = null
    }

    fun writeLed(json: String, onResult: (Boolean) -> Unit) {
        val characteristic = ledChar
        if (characteristic == null) {
            onResult(false)
            return
        }
        writeCharacteristic(
            characteristic,
            json.toByteArray(Charsets.UTF_8),
            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        )
            .done { onResult(true) }
            .fail { _, status ->
                Log.w(TAG, "LED write failed: status=$status")
                onResult(false)
            }
            .enqueue()
    }

    fun writeScreen(json: String, onResult: (Boolean) -> Unit) {
        val characteristic = screenChar
        if (characteristic == null) {
            onResult(false)
            return
        }
        writeCharacteristic(
            characteristic,
            json.toByteArray(Charsets.UTF_8),
            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        )
            .done { onResult(true) }
            .fail { _, status ->
                Log.w(TAG, "screen write failed: status=$status")
                onResult(false)
            }
            .enqueue()
    }

    fun writeHid(json: String, onResult: (Boolean) -> Unit) {
        val characteristic = hidChar
        if (characteristic == null) {
            onResult(false)
            return
        }
        writeCharacteristic(
            characteristic,
            json.toByteArray(Charsets.UTF_8),
            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        )
            .done { onResult(true) }
            .fail { _, status ->
                Log.w(TAG, "hid write failed: status=$status")
                onResult(false)
            }
            .enqueue()
    }

    fun isHidAvailable(): Boolean = hidChar != null

    fun isWifiAvailable(): Boolean = wifiChar != null

    /**
     * One provisioning command: {"cmd":"scan"} / {"cmd":"set","ssid":…,"password":…}
     * / {"cmd":"forget"}. A false result means the board refused it (bad JSON,
     * over-long SSID or passphrase, a scan already running) or the link died.
     */
    fun writeWifi(json: String, onResult: (Boolean) -> Unit) {
        val characteristic = wifiChar
        if (characteristic == null) {
            onResult(false)
            return
        }
        writeCharacteristic(
            characteristic,
            json.toByteArray(Charsets.UTF_8),
            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
        )
            .done { onResult(true) }
            .fail { _, status ->
                Log.w(TAG, "wifi write failed: status=$status")
                onResult(false)
            }
            .enqueue()
    }

    /** Reads the config + last scan; delivered through [Listener.onWifiConfig]. */
    @SuppressLint("MissingPermission")
    fun readWifiConfig() {
        val characteristic = wifiChar ?: return
        readCharacteristic(characteristic)
            .with { _, data ->
                data.value?.let { listener?.onWifiConfig(String(it, Charsets.UTF_8)) }
            }
            .fail { _, status -> Log.w(TAG, "wifi config read failed: status=$status") }
            .enqueue()
    }

    @SuppressLint("MissingPermission")
    fun readLedCtl() {
        val characteristic = ledChar ?: return
        readCharacteristic(characteristic)
            .with { _, data -> data.value?.let { listener?.onLedCtl(String(it, Charsets.UTF_8)) } }
            .fail { _, status -> Log.w(TAG, "ctl read failed: status=$status") }
            .enqueue()
    }

    @SuppressLint("MissingPermission")
    fun readStatus() {
        val characteristic = statusChar ?: return
        readCharacteristic(characteristic)
            .with { _, data -> data.value?.let { listener?.onLedStatus(String(it, Charsets.UTF_8)) } }
            .fail { _, status -> Log.w(TAG, "status read failed: status=$status") }
            .enqueue()
    }

    companion object {
        private const val TAG = "LedManager"

        val SERVICE_UUID: UUID = UUID.fromString("5d400001-7a9b-4c8e-b2f4-38a1c6e0d951")
        val LED_CHAR_UUID: UUID = UUID.fromString("5d400002-7a9b-4c8e-b2f4-38a1c6e0d951")
        val STATUS_CHAR_UUID: UUID = UUID.fromString("5d400003-7a9b-4c8e-b2f4-38a1c6e0d951")
        val SCREEN_CHAR_UUID: UUID = UUID.fromString("5d400004-7a9b-4c8e-b2f4-38a1c6e0d951")
        val HID_CHAR_UUID: UUID = UUID.fromString("5d400007-7a9b-4c8e-b2f4-38a1c6e0d951")
        val WIFI_CHAR_UUID: UUID = UUID.fromString("5d400008-7a9b-4c8e-b2f4-38a1c6e0d951")
    }
}
