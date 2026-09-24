// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "haptics-output.h"

#include <chiaki/log.h>

#include <aaudio/AAudio.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// The console's haptics are 3 kHz, the DualSense plays 48 kHz on 4 channels:
// 1 and 2 are the headphone jack, 3 and 4 the left and right actuator.
// Android only mixes USB audio in stereo, which drops channels 3 and 4, but its direct output
// takes 5.1, where the back left and right channels become the device's channels 3 and 4.
#define HAPTICS_INPUT_RATE 3000
#define HAPTICS_OUTPUT_RATE 48000
#define HAPTICS_UPSAMPLE (HAPTICS_OUTPUT_RATE / HAPTICS_INPUT_RATE)
#define HAPTICS_CHANNELS 6
#define HAPTICS_CHANNEL_LEFT 4 // back left
#define HAPTICS_CHANNEL_RIGHT 5 // back right

// In frames at the output rate, the buffer absorbs the network's jitter,
// but more than the maximum is dropped to keep the haptics in sync with the picture
#define RING_SIZE 8192
#define PREBUFFER_FRAMES (HAPTICS_OUTPUT_RATE / 100) // 10 ms
#define MAX_BUFFERED_FRAMES (HAPTICS_OUTPUT_RATE / 20) // 50 ms
#define TARGET_BUFFERED_FRAMES (HAPTICS_OUTPUT_RATE / 50) // 20 ms

// Rumble motors are emulated with a low tone for the big left motor and a higher one for the small right motor
#define RUMBLE_LEFT_HZ 80.0
#define RUMBLE_RIGHT_HZ 160.0
#define RUMBLE_AMPLITUDE 24000.0

// The haptics come out much weaker than on the console, part of it is lost to the 5.1 to 4 channel conversion.
// Louder parts are compressed instead of clipped, which would buzz.
#define HAPTICS_GAIN 3.0
#define HAPTICS_KNEE 20000.0

// AAudio is loaded at runtime, it doesn't exist before Android 8 and channel masks before Android 12L
struct AAudioFunctions
{
	aaudio_result_t (*createStreamBuilder)(AAudioStreamBuilder **builder);
	void (*setDeviceId)(AAudioStreamBuilder *builder, int32_t device_id);
	void (*setSampleRate)(AAudioStreamBuilder *builder, int32_t sample_rate);
	void (*setChannelMask)(AAudioStreamBuilder *builder, aaudio_channel_mask_t channel_mask);
	void (*setFormat)(AAudioStreamBuilder *builder, aaudio_format_t format);
	void (*setSharingMode)(AAudioStreamBuilder *builder, aaudio_sharing_mode_t sharing_mode);
	void (*setPerformanceMode)(AAudioStreamBuilder *builder, aaudio_performance_mode_t mode);
	void (*setUsage)(AAudioStreamBuilder *builder, aaudio_usage_t usage);
	void (*setDataCallback)(AAudioStreamBuilder *builder, AAudioStream_dataCallback callback, void *user_data);
	void (*setErrorCallback)(AAudioStreamBuilder *builder, AAudioStream_errorCallback callback, void *user_data);
	aaudio_result_t (*openStream)(AAudioStreamBuilder *builder, AAudioStream **stream);
	aaudio_result_t (*builderDelete)(AAudioStreamBuilder *builder);
	aaudio_result_t (*requestStart)(AAudioStream *stream);
	aaudio_result_t (*close)(AAudioStream *stream);
	int32_t (*getDeviceId)(AAudioStream *stream);
	int32_t (*getChannelCount)(AAudioStream *stream);
	int32_t (*getSampleRate)(AAudioStream *stream);
	aaudio_format_t (*getFormat)(AAudioStream *stream);
	const char *(*convertResultToText)(aaudio_result_t result);
	bool loaded = false;
};

static AAudioFunctions *aaudio()
{
	static AAudioFunctions f;
	static std::once_flag once;
	std::call_once(once, [] {
		void *lib = dlopen("libaaudio.so", RTLD_NOW);
		if(!lib)
			return;
		bool ok = true;
		auto load = [&](auto &fn, const char *name) {
			fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
			ok = ok && fn;
		};
		load(f.createStreamBuilder, "AAudio_createStreamBuilder");
		load(f.setDeviceId, "AAudioStreamBuilder_setDeviceId");
		load(f.setSampleRate, "AAudioStreamBuilder_setSampleRate");
		load(f.setChannelMask, "AAudioStreamBuilder_setChannelMask");
		load(f.setFormat, "AAudioStreamBuilder_setFormat");
		load(f.setSharingMode, "AAudioStreamBuilder_setSharingMode");
		load(f.setPerformanceMode, "AAudioStreamBuilder_setPerformanceMode");
		load(f.setUsage, "AAudioStreamBuilder_setUsage");
		load(f.setDataCallback, "AAudioStreamBuilder_setDataCallback");
		load(f.setErrorCallback, "AAudioStreamBuilder_setErrorCallback");
		load(f.openStream, "AAudioStreamBuilder_openStream");
		load(f.builderDelete, "AAudioStreamBuilder_delete");
		load(f.requestStart, "AAudioStream_requestStart");
		load(f.close, "AAudioStream_close");
		load(f.getDeviceId, "AAudioStream_getDeviceId");
		load(f.getChannelCount, "AAudioStream_getChannelCount");
		load(f.getSampleRate, "AAudioStream_getSampleRate");
		load(f.getFormat, "AAudioStream_getFormat");
		load(f.convertResultToText, "AAudio_convertResultToText");
		f.loaded = ok;
	});
	return f.loaded ? &f : nullptr;
}

struct HapticsFrame
{
	int16_t left;
	int16_t right;
};

struct HapticsOutput
{
	ChiakiLog *log;
	std::mutex stream_mutex;
	AAudioStream *stream = nullptr;
	int32_t device_id = 0;
	std::atomic<bool> active{false};

	// Single producer (the stream's haptics) and single consumer (the AAudio callback)
	HapticsFrame ring[RING_SIZE];
	std::atomic<size_t> write_pos{0};
	std::atomic<size_t> read_pos{0};
	HapticsFrame prev_input = {0, 0};
	bool buffering = true; // only touched by the consumer

	std::atomic<uint8_t> rumble_left{0};
	std::atomic<uint8_t> rumble_right{0};
	double rumble_phase_left = 0.0, rumble_phase_right = 0.0;

	// Loudest haptics sample so far, logged when it grows, to tell the levels that arrive
	int16_t input_peak = 0;

	// Out of 3, like ChiakiDualSenseEffectIntensity: Strong 3, Medium 2, Weak 1, Off 0
	std::atomic<int> scale{3};
};

static aaudio_data_callback_result_t data_callback(AAudioStream *stream, void *user, void *audio_data, int32_t num_frames);
static void error_callback(AAudioStream *stream, void *user, aaudio_result_t error);

// ho->stream_mutex must be locked
static void close_stream(HapticsOutput *ho)
{
	ho->active = false;
	if(!ho->stream)
		return;
	aaudio()->close(ho->stream);
	ho->stream = nullptr;
}

// ho->stream_mutex must be locked
static void open_stream(HapticsOutput *ho)
{
	close_stream(ho);
	if(!ho->device_id)
		return;
	auto a = aaudio();
	if(!a)
	{
		CHIAKI_LOGW(ho->log, "Haptics Output needs AAudio with channel masks (Android 12L)");
		return;
	}

	AAudioStreamBuilder *builder;
	aaudio_result_t result = a->createStreamBuilder(&builder);
	if(result != AAUDIO_OK)
	{
		CHIAKI_LOGE(ho->log, "Haptics Output failed to create AAudio builder: %s", a->convertResultToText(result));
		return;
	}
	a->setDeviceId(builder, ho->device_id);
	a->setSampleRate(builder, HAPTICS_OUTPUT_RATE);
	a->setChannelMask(builder, AAUDIO_CHANNEL_5POINT1);
	a->setFormat(builder, AAUDIO_FORMAT_PCM_I16);
	a->setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
	// Low latency would ask for a mixer, which is stereo only
	a->setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
	a->setUsage(builder, AAUDIO_USAGE_GAME);
	a->setDataCallback(builder, data_callback, ho);
	a->setErrorCallback(builder, error_callback, ho);
	result = a->openStream(builder, &ho->stream);
	a->builderDelete(builder);
	if(result != AAUDIO_OK)
	{
		ho->stream = nullptr;
		CHIAKI_LOGE(ho->log, "Haptics Output failed to open AAudio stream: %s", a->convertResultToText(result));
		return;
	}

	CHIAKI_LOGI(ho->log, "Haptics Output opened AAudio stream on device %d, %d channels, %d Hz",
			(int)a->getDeviceId(ho->stream), (int)a->getChannelCount(ho->stream), (int)a->getSampleRate(ho->stream));
	// The device id is only a wish, the haptics must never end up on the speakers
	if(a->getDeviceId(ho->stream) != ho->device_id
		|| a->getChannelCount(ho->stream) != HAPTICS_CHANNELS
		|| a->getSampleRate(ho->stream) != HAPTICS_OUTPUT_RATE
		|| a->getFormat(ho->stream) != AAUDIO_FORMAT_PCM_I16)
	{
		CHIAKI_LOGE(ho->log, "Haptics Output got a different device or format than requested, not playing haptics");
		close_stream(ho);
		return;
	}

	result = a->requestStart(ho->stream);
	if(result != AAUDIO_OK)
	{
		CHIAKI_LOGE(ho->log, "Haptics Output failed to start AAudio stream: %s", a->convertResultToText(result));
		close_stream(ho);
		return;
	}
	ho->active = true;
}

extern "C" void *android_chiaki_haptics_output_new(ChiakiLog *log)
{
	auto r = new HapticsOutput();
	r->log = log;
	return r;
}

extern "C" void android_chiaki_haptics_output_free(void *haptics_output)
{
	if(!haptics_output)
		return;
	auto ho = reinterpret_cast<HapticsOutput *>(haptics_output);
	{
		std::lock_guard<std::mutex> lock(ho->stream_mutex);
		close_stream(ho);
	}
	delete ho;
}

extern "C" void android_chiaki_haptics_output_set_device(int32_t device_id, void *haptics_output)
{
	auto ho = reinterpret_cast<HapticsOutput *>(haptics_output);
	std::lock_guard<std::mutex> lock(ho->stream_mutex);
	if(device_id == ho->device_id)
		return;
	ho->device_id = device_id;
	open_stream(ho);
}

extern "C" bool android_chiaki_haptics_output_active(void *haptics_output)
{
	return reinterpret_cast<HapticsOutput *>(haptics_output)->active;
}

extern "C" void android_chiaki_haptics_output_frame(uint8_t *buf, size_t buf_size, void *haptics_output)
{
	auto ho = reinterpret_cast<HapticsOutput *>(haptics_output);
	if(!ho->active)
		return;
	size_t write_pos = ho->write_pos.load(std::memory_order_relaxed);
	size_t read_pos = ho->read_pos.load(std::memory_order_acquire);
	size_t samples = buf_size / sizeof(HapticsFrame);
	for(size_t i = 0; i < samples; i++)
	{
		HapticsFrame input;
		memcpy(&input, buf + i * sizeof(HapticsFrame), sizeof(HapticsFrame));
		int16_t peak = (int16_t)std::max(abs(input.left), abs(input.right));
		if(peak > ho->input_peak + ho->input_peak / 8 + 64)
		{
			ho->input_peak = peak;
			CHIAKI_LOGI(ho->log, "Haptics Output loudest haptics so far: %d%% of full scale", peak * 100 / 32768);
		}
		// Linear interpolation from the previous sample
		for(int j = 1; j <= HAPTICS_UPSAMPLE; j++)
		{
			if(write_pos - read_pos >= RING_SIZE)
				break; // full, the consumer isn't running
			HapticsFrame &out = ho->ring[write_pos % RING_SIZE];
			out.left = (int16_t)(ho->prev_input.left + (input.left - ho->prev_input.left) * j / HAPTICS_UPSAMPLE);
			out.right = (int16_t)(ho->prev_input.right + (input.right - ho->prev_input.right) * j / HAPTICS_UPSAMPLE);
			write_pos++;
		}
		ho->prev_input = input;
	}
	ho->write_pos.store(write_pos, std::memory_order_release);
}

extern "C" void android_chiaki_haptics_output_rumble(uint8_t left, uint8_t right, void *haptics_output)
{
	auto ho = reinterpret_cast<HapticsOutput *>(haptics_output);
	ho->rumble_left = left;
	ho->rumble_right = right;
}

extern "C" void android_chiaki_haptics_output_intensity(int intensity, void *haptics_output)
{
	auto ho = reinterpret_cast<HapticsOutput *>(haptics_output);
	CHIAKI_LOGI(ho->log, "Haptics Output intensity set by the console: %d", intensity);
	switch(intensity)
	{
		case 0: ho->scale = 0; break; // Off
		case 1: ho->scale = 3; break; // Strong
		case 2: ho->scale = 2; break; // Medium
		case 3: ho->scale = 1; break; // Weak
		default: break;
	}
}

static inline int16_t clamp_sample(int32_t v)
{
	return (int16_t)(v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : v));
}

// Gain, and above the knee a smooth curve that approaches full scale
static inline int16_t shape_sample(int32_t v)
{
	double x = v * HAPTICS_GAIN;
	double a = fabs(x);
	if(a > HAPTICS_KNEE)
	{
		const double range = INT16_MAX - HAPTICS_KNEE;
		a = HAPTICS_KNEE + range * tanh((a - HAPTICS_KNEE) / range);
	}
	return (int16_t)(x < 0 ? -a : a);
}

static aaudio_data_callback_result_t data_callback(AAudioStream *stream, void *user, void *audio_data, int32_t num_frames)
{
	auto ho = reinterpret_cast<HapticsOutput *>(user);
	auto out = reinterpret_cast<int16_t *>(audio_data);

	size_t write_pos = ho->write_pos.load(std::memory_order_acquire);
	size_t read_pos = ho->read_pos.load(std::memory_order_relaxed);
	size_t buffered = write_pos - read_pos;
	if(ho->buffering && buffered >= PREBUFFER_FRAMES)
		ho->buffering = false;
	if(buffered > MAX_BUFFERED_FRAMES)
		read_pos = write_pos - TARGET_BUFFERED_FRAMES;

	int scale = ho->scale;
	double rumble_left = ho->rumble_left * (RUMBLE_AMPLITUDE / 255.0);
	double rumble_right = ho->rumble_right * (RUMBLE_AMPLITUDE / 255.0);
	const double step_left = 2.0 * M_PI * RUMBLE_LEFT_HZ / HAPTICS_OUTPUT_RATE;
	const double step_right = 2.0 * M_PI * RUMBLE_RIGHT_HZ / HAPTICS_OUTPUT_RATE;

	for(int32_t i = 0; i < num_frames; i++)
	{
		int32_t left = 0, right = 0;
		int32_t rumble_l = 0, rumble_r = 0;
		if(!ho->buffering)
		{
			if(read_pos != write_pos)
			{
				const HapticsFrame &frame = ho->ring[read_pos % RING_SIZE];
				left = frame.left;
				right = frame.right;
				read_pos++;
			}
			else
				ho->buffering = true; // ran dry, wait for enough again
		}
		if(rumble_left > 0.0)
		{
			rumble_l = (int32_t)(rumble_left * sin(ho->rumble_phase_left));
			ho->rumble_phase_left = fmod(ho->rumble_phase_left + step_left, 2.0 * M_PI);
		}
		if(rumble_right > 0.0)
		{
			rumble_r = (int32_t)(rumble_right * sin(ho->rumble_phase_right));
			ho->rumble_phase_right = fmod(ho->rumble_phase_right + step_right, 2.0 * M_PI);
		}
		int16_t *o = out + i * HAPTICS_CHANNELS;
		memset(o, 0, HAPTICS_CHANNELS * sizeof(int16_t));
		o[HAPTICS_CHANNEL_LEFT] = clamp_sample((shape_sample(left) + rumble_l) * scale / 3);
		o[HAPTICS_CHANNEL_RIGHT] = clamp_sample((shape_sample(right) + rumble_r) * scale / 3);
	}

	ho->read_pos.store(read_pos, std::memory_order_release);
	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void error_callback(AAudioStream *stream, void *user, aaudio_result_t error)
{
	auto ho = reinterpret_cast<HapticsOutput *>(user);
	CHIAKI_LOGE(ho->log, "Haptics Output AAudio error: %s", aaudio()->convertResultToText(error));
	// The stream can't be closed from this callback
	std::thread([ho, stream, error] {
		std::lock_guard<std::mutex> lock(ho->stream_mutex);
		if(stream != ho->stream)
			return; // already replaced
		// e.g. rerouted because another device came up, the controller might still be there
		if(error == AAUDIO_ERROR_DISCONNECTED)
			open_stream(ho);
		else
			close_stream(ho);
	}).detach();
}
