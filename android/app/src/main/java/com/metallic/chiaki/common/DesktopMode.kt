// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.common

import android.app.Activity
import android.content.Context
import android.os.Build
import android.view.Display

/**
 * Samsung DeX, where apps run in windows on a desktop (on an external display).
 */
fun Context.isSamsungDex(): Boolean = try
{
	val config = resources.configuration
	val configClass = config.javaClass
	configClass.getField("SEM_DESKTOP_MODE_ENABLED").getInt(null) == configClass.getField("semDesktopModeEnabled").getInt(config)
}
catch(e: Exception)
{
	false // not a Samsung device
}

/**
 * Samsung DeX, other desktop modes, external displays or multi-window,
 * where the stream should be a freely resizable window instead of locked to landscape.
 */
fun Activity.isDesktopMode(): Boolean
{
	if(isSamsungDex() || isInMultiWindowMode)
		return true
	@Suppress("DEPRECATION")
	val displayId = if(Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) display?.displayId else windowManager.defaultDisplay.displayId
	return displayId != null && displayId != Display.DEFAULT_DISPLAY
}
