// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_JNI_AUDIO_OUTPUT_H
#define CHIAKI_JNI_AUDIO_OUTPUT_H

#include <chiaki/log.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *android_chiaki_audio_output_new(ChiakiLog *log);
void android_chiaki_audio_output_free(void *audio_output);
void android_chiaki_audio_output_settings(uint32_t channels, uint32_t rate, void *audio_output);
/**
 * @param device_id AudioDeviceInfo id to play on, 0 for the system's choice
 */
void android_chiaki_audio_output_set_device(int32_t device_id, void *audio_output);
void android_chiaki_audio_output_frame(int16_t *buf, size_t samples_count, void *audio_output);

#ifdef __cplusplus
}
#endif

#endif //CHIAKI_JNI_AUDIO_OUTPUT_H
