/**
 * @file
 *
 * @date 14.05.2016
 * @author Anton Bondarev
 */
#include <stddef.h>
#include <assert.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>

#include <util/log.h>

#include <kernel/lthread/lthread.h>

#include <drivers/audio/portaudio.h>
#include <drivers/audio/audio_dev.h>

#include "es1370.h"

static struct lthread portaudio_lthread;

struct pa_strm {
	uint8_t devid;
	uint8_t number_of_chan;
	uint32_t sample_format;

	PaStreamCallback *callback;
	void *user_data;
	//uint32_t samples_per_buffer;

	int active;
};

static struct pa_strm pa_stream;

static int sample_format_in_bytes(uint32_t pa_format) {
	switch (pa_format) {
	case paInt16:
		return 2;
	case paInt8:
		return 1;
	default:
		log_error("Unsupport stream format");
		return -EINVAL;
	}
	return -EINVAL;
}

static int _bytes_per_sample(struct pa_strm *stream) {
	return stream->number_of_chan *
	       sample_format_in_bytes(stream->sample_format);
}



static int portaudio_lthread_handle(struct lthread *self) {
	int buf_len;
	int retval;
	struct audio_dev *audio_dev;
	uint8_t *out_buf;

	if (!pa_stream.callback || !pa_stream.active) {
		return 0;
	}

	audio_dev = audio_dev_get_by_idx(pa_stream.devid);
	out_buf = audio_dev_get_out_cur_ptr(audio_dev);

	retval = pa_stream.callback(NULL,
			out_buf,
			audio_dev->samples_per_buffer,
			NULL,
			0,
			pa_stream.user_data);

	buf_len = audio_dev->samples_per_buffer * _bytes_per_sample(&pa_stream);
	es1370_update_dma(buf_len, pa_stream.devid);

	if (retval != paContinue)
		pa_stream.active = 0;

	audio_dev->ad_ops->ad_ops_resume(audio_dev);

	return 0;
}

PaError Pa_Initialize(void) {
	lthread_init(&portaudio_lthread, portaudio_lthread_handle);
	return paNoError;
}

PaError Pa_Terminate(void) {
	return paNoError;
}

PaHostApiIndex Pa_GetHostApiCount(void) { return 1; }
PaDeviceIndex Pa_GetDeviceCount(void) { return 1; }
PaDeviceIndex Pa_GetDefaultOutputDevice(void) { return 0; }

const char * Pa_GetErrorText(PaError errorCode) {
	return "Pa_GetErrorText not implemented";
}

const PaDeviceInfo * Pa_GetDeviceInfo(PaDeviceIndex device) {
	static const PaDeviceInfo info = {
		.structVersion = 1,
		.name = "es1370 portaudio",
		.hostApi = 0,
		.maxInputChannels = 1,
		.maxOutputChannels = 1,
		.defaultLowInputLatency = 0,
		.defaultLowOutputLatency = 0,
		.defaultHighInputLatency = 0,
		.defaultHighOutputLatency = 0,
		.defaultSampleRate = 44100
	};
	const PaDeviceInfo *pa_info = device == 0 ? &info : NULL;

	return pa_info;
}

const PaHostApiInfo * Pa_GetHostApiInfo(PaHostApiIndex hostApi) {
	return NULL;
}

const PaStreamInfo * Pa_GetStreamInfo(PaStream *stream) {
	static PaStreamInfo info = {
		.structVersion = 1,
		.inputLatency = 0,
		.outputLatency = 0,
		.sampleRate = 44100
	};
	PaStreamInfo *pa_info = stream != NULL ? &info : NULL;

	return pa_info;
}


PaError Pa_OpenStream(PaStream** stream,
		const PaStreamParameters *inputParameters,
		const PaStreamParameters *outputParameters,
		double sampleRate, unsigned long framesPerBuffer,
		PaStreamFlags streamFlags, PaStreamCallback *streamCallback,
		void *userData) {
	struct audio_dev *audio_dev;

	assert(stream != NULL);
	assert(streamFlags == paNoFlag || streamFlags == paClipOff);
	assert(streamCallback != NULL);

	log_debug("stream %p input %p output %p rate %f"
			" framesPerBuffer %lu flags %lu callback %p user_data %p",
			stream, inputParameters, outputParameters, sampleRate,
			framesPerBuffer, streamFlags, streamCallback, userData);

	pa_stream.number_of_chan = outputParameters->channelCount;
	pa_stream.devid = outputParameters->device;
	pa_stream.sample_format = outputParameters->sampleFormat;
	//pa_stream.samples_per_buffer = framesPerBuffer;
	pa_stream.callback = streamCallback;
	pa_stream.user_data = userData;
	pa_stream.active = 1;

	*stream = &pa_stream;

	audio_dev = audio_dev_get_by_idx(pa_stream.devid);
	audio_dev->samples_per_buffer = framesPerBuffer;
	audio_dev->ad_ops->ad_ops_start(audio_dev);
	audio_dev->num_of_chan = outputParameters->channelCount;

	return paNoError;
}

PaError Pa_CloseStream(PaStream *stream) {
	struct pa_strm *strm;
	struct audio_dev *audio_dev;

	strm = (struct pa_strm *)stream;

	audio_dev = audio_dev_get_by_idx(strm->devid);
	audio_dev->ad_ops->ad_ops_pause(audio_dev);

	return paNoError;
}

PaError Pa_StartStream(PaStream *stream) {

	lthread_launch(&portaudio_lthread);

	return paNoError;
}

PaError Pa_StopStream(PaStream *stream) {
	return paNoError;
}

void Pa_Sleep(long msec) {
	usleep(msec * 1000);
}