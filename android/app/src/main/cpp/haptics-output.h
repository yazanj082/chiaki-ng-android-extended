// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_JNI_HAPTICS_OUTPUT_H
#define CHIAKI_JNI_HAPTICS_OUTPUT_H

#include <chiaki/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Plays DualSense haptics on the controller's voice coil actuators, through the controller's
 * USB audio device, where channels 3 and 4 drive the left and right actuator.
 * Only possible while the DualSense is connected by USB.
 */
void *android_chiaki_haptics_output_new(ChiakiLog *log);
void android_chiaki_haptics_output_free(void *haptics_output);

/**
 * @param device_id AudioDeviceInfo id of the DualSense's USB audio, 0 to stop
 */
void android_chiaki_haptics_output_set_device(int32_t device_id, void *haptics_output);

/**
 * @return whether the haptics are played on the controller, false while no stream is open
 */
bool android_chiaki_haptics_output_active(void *haptics_output);

/**
 * Haptics from the console, 3 kHz stereo 16 bit PCM
 */
void android_chiaki_haptics_output_frame(uint8_t *buf, size_t buf_size, void *haptics_output);

/**
 * Classic rumble (e.g. from PS4 games), played as vibration of the actuators
 */
void android_chiaki_haptics_output_rumble(uint8_t left, uint8_t right, void *haptics_output);

/**
 * @param intensity ChiakiDualSenseEffectIntensity chosen on the console
 */
void android_chiaki_haptics_output_intensity(int intensity, void *haptics_output);

#ifdef __cplusplus
}
#endif

#endif //CHIAKI_JNI_HAPTICS_OUTPUT_H
