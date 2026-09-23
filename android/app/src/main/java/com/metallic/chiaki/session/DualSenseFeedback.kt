// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.session

import android.util.Log
import com.metallic.chiaki.lib.DualSenseIntensity
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.util.concurrent.Executors
import java.util.zip.CRC32

/**
 * Adaptive triggers and the lightbar of DualSense controllers, which Android has no API for.
 * They are driven with raw output reports written to the controller's /dev/hidraw* node.
 * That needs read/write access to hidraw nodes, which the PS Remote Play Orange Pi image
 * grants; elsewhere this does nothing.
 */
class DualSenseFeedback
{
	companion object
	{
		private const val TAG = "DualSenseFeedback"

		private const val VENDOR_SONY = 0x054c
		private val PRODUCTS_DUALSENSE = setOf(0x0ce6, 0x0df2) // DualSense, DualSense Edge
		private const val BUS_USB = 0x03
		private const val BUS_BLUETOOTH = 0x05

		private const val REPORT_USB = 0x02
		private const val REPORT_USB_SIZE = 63
		private const val REPORT_BT = 0x31
		private const val REPORT_BT_SIZE = 78
		private const val REPORT_BT_TAG = 0x10
		private const val REPORT_BT_CRC_SEED = 0xa2

		// Offsets in the report part that is common to USB and Bluetooth
		private const val VALID_FLAG0 = 0
		private const val VALID_FLAG1 = 1
		private const val RIGHT_TRIGGER = 10
		private const val LEFT_TRIGGER = 21
		private const val LIGHTBAR_RED = 44

		private const val VALID_FLAG0_RIGHT_TRIGGER = 0x04
		private const val VALID_FLAG0_LEFT_TRIGGER = 0x08
		private const val VALID_FLAG1_LIGHTBAR = 0x04

		private const val TRIGGER_EFFECT_OFF = 0x05
		private const val TRIGGER_EFFECT_SIZE = 11
	}

	private class Controller(val node: String, val bluetooth: Boolean)
	{
		var output: FileOutputStream? = null
		var sequence = 0
	}

	// Everything touching the controllers runs on this thread, in order
	private val executor = Executors.newSingleThreadExecutor()
	private var controllers: List<Controller>? = null

	private val rightTrigger = ByteArray(TRIGGER_EFFECT_SIZE).also { it[0] = TRIGGER_EFFECT_OFF.toByte() }
	private val leftTrigger = ByteArray(TRIGGER_EFFECT_SIZE).also { it[0] = TRIGGER_EFFECT_OFF.toByte() }
	private var triggersChanged = false
	private var lightbar: Triple<Int, Int, Int>? = null
	private var triggerIntensity = DualSenseIntensity.STRONG

	fun setTriggerEffects(typeLeft: Int, typeRight: Int, left: ByteArray, right: ByteArray) = enqueue {
		leftTrigger[0] = typeLeft.toByte()
		left.copyInto(leftTrigger, 1, 0, minOf(left.size, TRIGGER_EFFECT_SIZE - 1))
		rightTrigger[0] = typeRight.toByte()
		right.copyInto(rightTrigger, 1, 0, minOf(right.size, TRIGGER_EFFECT_SIZE - 1))
		triggersChanged = true
		send()
	}

	fun setTriggerIntensity(intensity: DualSenseIntensity) = enqueue {
		triggerIntensity = intensity
		triggersChanged = true
		send()
	}

	fun setLightbar(red: Int, green: Int, blue: Int) = enqueue {
		lightbar = Triple(red, green, blue)
		send()
	}

	/**
	 * Controllers were connected or disconnected, look for them again with the next update.
	 */
	fun rescan() = enqueue {
		closeAll()
		controllers = null
	}

	/**
	 * Releases the triggers, so they don't stay stiff when the stream ends.
	 */
	fun reset() = enqueue {
		rightTrigger.fill(0)
		rightTrigger[0] = TRIGGER_EFFECT_OFF.toByte()
		leftTrigger.fill(0)
		leftTrigger[0] = TRIGGER_EFFECT_OFF.toByte()
		triggersChanged = true
		lightbar = null
		send()
	}

	fun close()
	{
		enqueue { closeAll() }
		executor.shutdown()
	}

	private fun enqueue(block: () -> Unit)
	{
		if(!executor.isShutdown)
			executor.execute(block)
	}

	private fun send()
	{
		val controllers = controllers ?: findControllers().also { controllers = it }
		for(controller in controllers)
		{
			val report = buildReport(controller)
			try
			{
				val output = controller.output ?: FileOutputStream(controller.node).also { controller.output = it }
				output.write(report)
			}
			catch(e: IOException)
			{
				// No access to hidraw on this device, or the controller is gone
				Log.i(TAG, "Can't send to ${controller.node}: ${e.message}")
				controller.output?.close()
				controller.output = null
			}
		}
		triggersChanged = false
	}

	private fun buildReport(controller: Controller): ByteArray
	{
		val report: ByteArray
		val common: Int
		if(controller.bluetooth)
		{
			report = ByteArray(REPORT_BT_SIZE)
			report[0] = REPORT_BT.toByte()
			// The upper nibble is a sequence number that has to increase with every report
			report[1] = (controller.sequence shl 4).toByte()
			controller.sequence = (controller.sequence + 1) and 0xf
			report[2] = REPORT_BT_TAG.toByte()
			common = 3
		}
		else
		{
			report = ByteArray(REPORT_USB_SIZE)
			report[0] = REPORT_USB.toByte()
			common = 1
		}

		if(triggersChanged)
		{
			report[common + VALID_FLAG0] = (VALID_FLAG0_RIGHT_TRIGGER or VALID_FLAG0_LEFT_TRIGGER).toByte()
			if(triggerIntensity == DualSenseIntensity.OFF)
			{
				report[common + RIGHT_TRIGGER] = TRIGGER_EFFECT_OFF.toByte()
				report[common + LEFT_TRIGGER] = TRIGGER_EFFECT_OFF.toByte()
			}
			else
			{
				rightTrigger.copyInto(report, common + RIGHT_TRIGGER)
				leftTrigger.copyInto(report, common + LEFT_TRIGGER)
			}
		}
		lightbar?.let { (red, green, blue) ->
			report[common + VALID_FLAG1] = VALID_FLAG1_LIGHTBAR.toByte()
			report[common + LIGHTBAR_RED] = red.toByte()
			report[common + LIGHTBAR_RED + 1] = green.toByte()
			report[common + LIGHTBAR_RED + 2] = blue.toByte()
		}

		if(controller.bluetooth)
		{
			val crc = CRC32().apply {
				update(REPORT_BT_CRC_SEED)
				update(report, 0, report.size - 4)
			}.value
			for(i in 0 until 4)
				report[report.size - 4 + i] = (crc shr (8 * i)).toByte()
		}
		return report
	}

	private fun findControllers(): List<Controller>
	{
		val nodes = File("/sys/class/hidraw").listFiles() ?: return emptyList()
		return nodes.mapNotNull { node ->
			try
			{
				// e.g. HID_ID=0005:0000054C:00000CE6
				val id = File(node, "device/uevent").readLines()
					.firstOrNull { it.startsWith("HID_ID=") }
					?.removePrefix("HID_ID=")
					?.split(":")
					?.map { it.toInt(16) }
					?: return@mapNotNull null
				val (bus, vendor, product) = id
				if(vendor != VENDOR_SONY || product !in PRODUCTS_DUALSENSE || (bus != BUS_USB && bus != BUS_BLUETOOTH))
					return@mapNotNull null
				Controller("/dev/${node.name}", bus == BUS_BLUETOOTH)
			}
			catch(e: Exception)
			{
				null
			}
		}.also { Log.i(TAG, "DualSense controllers: ${it.map { c -> c.node }}") }
	}

	private fun closeAll()
	{
		controllers?.forEach {
			try { it.output?.close() } catch(e: IOException) { }
			it.output = null
		}
	}
}
