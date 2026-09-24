// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.session

import android.content.Context
import android.media.AudioDeviceCallback
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Handler
import android.os.Looper

/**
 * Picks the output device for the stream's audio.
 *
 * A DualSense or DualShock 4 on USB is also a USB headset, which Android then plays everything on,
 * so the sound is gone unless headphones are plugged into the controller.
 * Unless those are wanted, the audio goes to a TV or monitor on HDMI (e.g. DeX) instead,
 * or else the phone's speaker.
 *
 * A DualSense's USB audio also drives its actuators, which then play the haptics.
 */
class AudioRouting(context: Context, private val controllerHeadphones: Boolean, private val changedCallback: () -> Unit)
{
	private val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager

	/** [AudioDeviceInfo.getId], or 0 for the system's choice */
	var deviceId = 0
		private set

	/** [AudioDeviceInfo.getId] of a DualSense on USB for the haptics, or 0 */
	var hapticsDeviceId = 0
		private set

	private val deviceCallback = object: AudioDeviceCallback()
	{
		override fun onAudioDevicesAdded(addedDevices: Array<out AudioDeviceInfo>?) = update()
		override fun onAudioDevicesRemoved(removedDevices: Array<out AudioDeviceInfo>?) = update()
	}

	init
	{
		// Also reports the current devices right away
		audioManager.registerAudioDeviceCallback(deviceCallback, Handler(Looper.getMainLooper()))
	}

	fun close()
	{
		audioManager.unregisterAudioDeviceCallback(deviceCallback)
	}

	private fun update()
	{
		val outputs = audioManager.getDevices(AudioManager.GET_DEVICES_OUTPUTS)
		val id = pickDevice(outputs)
		val hapticsId = outputs.firstOrNull { isDualSenseWithActuators(it) }?.id ?: 0
		if(id == deviceId && hapticsId == hapticsDeviceId)
			return
		deviceId = id
		hapticsDeviceId = hapticsId
		changedCallback()
	}

	private fun pickDevice(outputs: Array<AudioDeviceInfo>): Int
	{
		if(controllerHeadphones || outputs.none { isController(it) })
			return 0
		// Headphones or speakers connected besides the controller keep the sound
		if(outputs.any { it.type in PERSONAL_TYPES && !isController(it) })
			return 0
		return (outputs.firstOrNull { it.type in HDMI_TYPES }
			?: outputs.firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER })?.id ?: 0
	}

	private fun isController(device: AudioDeviceInfo) =
		(device.type == AudioDeviceInfo.TYPE_USB_HEADSET || device.type == AudioDeviceInfo.TYPE_USB_DEVICE)
			&& device.productName.contains("Wireless Controller") // e.g. "USB-Audio - DualSense Wireless Controller"

	// Channels 3 and 4 are the actuators
	private fun isDualSenseWithActuators(device: AudioDeviceInfo) =
		isController(device) && device.productName.contains("DualSense")
			&& (device.channelCounts.any { it >= 4 } || device.channelIndexMasks.any { Integer.bitCount(it) >= 4 })

	companion object
	{
		private val HDMI_TYPES = setOf(AudioDeviceInfo.TYPE_HDMI, AudioDeviceInfo.TYPE_HDMI_ARC, 29 /* TYPE_HDMI_EARC */)
		private val PERSONAL_TYPES = setOf(
			AudioDeviceInfo.TYPE_WIRED_HEADSET,
			AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
			AudioDeviceInfo.TYPE_BLUETOOTH_A2DP,
			AudioDeviceInfo.TYPE_USB_HEADSET,
			AudioDeviceInfo.TYPE_USB_DEVICE,
			26 /* TYPE_BLE_HEADSET */,
			27 /* TYPE_BLE_SPEAKER */)
	}
}
