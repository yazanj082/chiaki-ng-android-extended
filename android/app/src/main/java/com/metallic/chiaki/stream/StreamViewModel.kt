// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.stream

import android.app.Application
import android.content.Context
import androidx.lifecycle.*
import com.metallic.chiaki.common.LogManager
import com.metallic.chiaki.session.StreamSession
import com.metallic.chiaki.common.Preferences
import com.metallic.chiaki.lib.*
import com.metallic.chiaki.session.StreamInput

class StreamViewModel(val application: Application, val connectInfo: ConnectInfo): ViewModel()
{
	val preferences = Preferences(application)
	val logManager = LogManager(application)

	val input = StreamInput(application, preferences)
	val session = StreamSession(connectInfo, logManager, preferences.logVerbose, input)

	private var controllerConnected = false
	// Turned on explicitly while a controller is connected, only for this stream
	private var onScreenControlsWithController = false

	private var _onScreenControlsVisible = MutableLiveData<Boolean>(preferences.onScreenControlsEnabled)
	/**
	 * Hidden while a physical controller is connected, e.g. in DeX, where the stream is on a TV that can't be touched
	 */
	val onScreenControlsVisible: LiveData<Boolean> get() = _onScreenControlsVisible


	override fun onCleared()
	{
		super.onCleared()
		session.release()
	}

	fun setOnScreenControlsEnabled(enabled: Boolean)
	{
		if(controllerConnected)
			onScreenControlsWithController = enabled
		else
			preferences.onScreenControlsEnabled = enabled
		updateOnScreenControlsVisible()
	}

	fun setControllerConnected(connected: Boolean)
	{
		controllerConnected = connected
		updateOnScreenControlsVisible()
	}

	private fun updateOnScreenControlsVisible()
	{
		val visible = if(controllerConnected) onScreenControlsWithController else preferences.onScreenControlsEnabled
		if(_onScreenControlsVisible.value != visible)
			_onScreenControlsVisible.value = visible
	}

}