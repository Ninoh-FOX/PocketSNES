#include <stdlib.h>
#include <SDL.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#include <sal.h>

#define BUFFER_FRAMES 3
// 48000 Hz maximum; 1/50 of a second; 3 frames to hold (2 plus a bit extra)
#define BUFFER_SAMPLES (48000 / 50 * (BUFFER_FRAMES + 1))

static SDL_AudioSpec audiospec;

volatile static unsigned int ReadPos, WritePos;

// 2 channels per sample (stereo); 2 bytes per sample-channel (16-bit)
static uint8_t Buffer[BUFFER_SAMPLES * 2 * 2];
static u32 SamplesPerFrame, BytesPerSample;
static u32 Muted; // S9xSetAudioMute(TRUE) gets undone after SNES Global Mute ends

/* The audio callback signals this after consuming samples, so the emulation
 * thread wakes up as soon as there is room again. usleep() polling is not
 * usable here: the Miyoo kernel (HZ=100) turns usleep(1000) into 10 ms. */
static pthread_mutex_t SpaceMutex = PTHREAD_MUTEX_INITIALIZER;
/* Without an audio device the frame rate is kept with the clock instead */
static int AudioOpen = 0;
static unsigned long long FrameTimeNs = 16666667ULL;
static struct timespec NextFrame;
static pthread_cond_t SpaceCond = PTHREAD_COND_INITIALIZER;

static void sdl_audio_callback (void *userdata, Uint8 *stream, int len)
{
	u32 SamplesRequested = len / BytesPerSample, SamplesBuffered,
	    LocalWritePos = WritePos /* isolate a bit against races with the main thread */,
	    LocalReadPos = ReadPos /* keep a non-volatile copy at hand */;
	if (LocalReadPos <= LocalWritePos)
		SamplesBuffered = LocalWritePos - LocalReadPos;
	else
		SamplesBuffered = BUFFER_SAMPLES - (LocalReadPos - LocalWritePos);

	if (SamplesRequested > SamplesBuffered)
	{
		return;
	}

	if (Muted)
	{
		memset(stream, 0, len);
	}
	else if (LocalReadPos + SamplesRequested > BUFFER_SAMPLES)
	{
		memmove(stream, &Buffer[LocalReadPos * BytesPerSample], (BUFFER_SAMPLES - LocalReadPos) * BytesPerSample);
		memmove(&stream[(BUFFER_SAMPLES - LocalReadPos) * BytesPerSample], &Buffer[0], (SamplesRequested - (BUFFER_SAMPLES - LocalReadPos)) * BytesPerSample);
	}
	else
	{
		memmove(stream, &Buffer[LocalReadPos * BytesPerSample], len);
	}
	ReadPos = (LocalReadPos + SamplesRequested) % BUFFER_SAMPLES;

	pthread_mutex_lock(&SpaceMutex);
	pthread_cond_signal(&SpaceCond);
	pthread_mutex_unlock(&SpaceMutex);
}

s32 sal_AudioInit(s32 rate, s32 bits, s32 stereo, s32 Hz)
{
	int buffer = 1;
	audiospec.freq = rate;
	audiospec.channels = (stereo + 1);
	audiospec.format = AUDIO_S16;

	audiospec.samples = (rate / Hz);
	if (!stereo && (audiospec.samples & 1))
		audiospec.samples--;

	 
	SamplesPerFrame = audiospec.samples;
	BytesPerSample = audiospec.channels * (bits >> 3);


	audiospec.callback = sdl_audio_callback;
	/* Must be a power of 2: RS-97, and on the Miyoo Mini with audiofix ON the
	 * SDL 1.2 OSS driver (through libpadsp) rejects any other fragment size
	 * ("Fragment size must be a power of two") and audio does not open. */
	while (buffer < audiospec.samples)
	{
		buffer*=2;
	}
	audiospec.samples = buffer;

	AudioOpen = 0;
	FrameTimeNs = 1000000000ULL / Hz;
	clock_gettime(CLOCK_MONOTONIC, &NextFrame);
	if (SDL_OpenAudio(&audiospec, NULL) < 0) {
		fprintf(stderr, "Unable to initialize audio: %s\n", SDL_GetError());
		return SAL_ERROR;
	}
	AudioOpen = 1;

	WritePos = ReadPos = 0;

	return SAL_OK;
}

void sal_AudioPause(void)
{
	SDL_PauseAudio(1);
}

void sal_AudioResume(void)
{
	SDL_PauseAudio(0);
}

void sal_AudioClose(void)
{
	SDL_CloseAudio();
}

#ifndef POCKETSNES_LIBRETRO
void sal_AudioGenerate(u32 samples)
{
	u32 SamplesAvailable,
	    LocalReadPos = ReadPos /* isolate a bit against races with the audio thread */,
	    LocalWritePos = WritePos /* keep a non-volatile copy at hand */;
	if (LocalReadPos <= LocalWritePos)
		SamplesAvailable = BUFFER_SAMPLES - (LocalWritePos - LocalReadPos);
	else
		SamplesAvailable = LocalReadPos - LocalWritePos;
	if (samples >= SamplesAvailable)
	{
		samples = SamplesAvailable - 1;
	}
	if (samples > BUFFER_SAMPLES - LocalWritePos)
	{
		sal_AudioGenerate(BUFFER_SAMPLES - LocalWritePos);
		sal_AudioGenerate(samples - (BUFFER_SAMPLES - LocalWritePos));
	}
	else
	{
		S9xMixSamples(&Buffer[LocalWritePos * BytesPerSample], samples * audiospec.channels);
		WritePos = (LocalWritePos + samples) % BUFFER_SAMPLES;
	}
}
#endif

/* Appends 'samples' sample frames (interleaved, already in the output
 * channel count) to the ring buffer; drops what does not fit. */
void sal_AudioWrite(const s16 *data, u32 samples)
{
	u32 SamplesAvailable, chunk,
	    LocalReadPos = ReadPos,
	    LocalWritePos = WritePos;

	if (BytesPerSample == 0)
		return;
	if (LocalReadPos <= LocalWritePos)
		SamplesAvailable = BUFFER_SAMPLES - (LocalWritePos - LocalReadPos);
	else
		SamplesAvailable = LocalReadPos - LocalWritePos;
	if (samples >= SamplesAvailable)
		samples = SamplesAvailable - 1;

	while (samples > 0)
	{
		chunk = BUFFER_SAMPLES - LocalWritePos;
		if (chunk > samples)
			chunk = samples;
		memcpy(&Buffer[LocalWritePos * BytesPerSample], data, chunk * BytesPerSample);
		data += chunk * (BytesPerSample / 2);
		samples -= chunk;
		LocalWritePos = (LocalWritePos + chunk) % BUFFER_SAMPLES;
	}
	WritePos = LocalWritePos;
}

u32 sal_AudioGetFramesBuffered()
{
	u32 SamplesBuffered,
	    LocalReadPos = ReadPos /* isolate a bit against races with the audio thread */,
	    LocalWritePos = WritePos /* keep a non-volatile copy at hand */;
	if (LocalReadPos <= LocalWritePos)
		SamplesBuffered = LocalWritePos - LocalReadPos;
	else
		SamplesBuffered = BUFFER_SAMPLES - (LocalReadPos - LocalWritePos);
	if (SamplesPerFrame == 0) /* audio not opened yet */
		return 0;
	return SamplesBuffered / SamplesPerFrame;
}

/* Blocks while the buffer already holds the maximum number of frames. */
void sal_AudioWaitSpace(void)
{
	if (!AudioOpen)
	{
		/* absolute deadlines: no drift, and exact on the 100 Hz kernel */
		struct timespec now;
		NextFrame.tv_nsec += FrameTimeNs;
		while (NextFrame.tv_nsec >= 1000000000) { NextFrame.tv_nsec -= 1000000000; NextFrame.tv_sec++; }
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > NextFrame.tv_sec + 1)
			NextFrame = now; /* fell far behind (menu, load): resync */
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &NextFrame, NULL);
		return;
	}
	pthread_mutex_lock(&SpaceMutex);
	while (sal_AudioGetFramesBuffered() >= sal_AudioGetMaxFrames())
	{
		/* Safety net (paused/broken audio device): never block > 50 ms. */
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 50 * 1000000;
		if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
		if (pthread_cond_timedwait(&SpaceCond, &SpaceMutex, &ts) == ETIMEDOUT)
			break;
	}
	pthread_mutex_unlock(&SpaceMutex);
}

u32 sal_AudioGetMinFrames()
{
	return BUFFER_FRAMES - 1;
}

u32 sal_AudioGetMaxFrames()
{
	return BUFFER_FRAMES;
}

u32 sal_AudioGetSamplesPerFrame()
{
	return SamplesPerFrame;
}

u32 sal_AudioGetBytesPerSample()
{
	return BytesPerSample;
}

void sal_AudioSetVolume(s32 l, s32 r)
{
}

void sal_AudioSetMuted(u32 muted)
{
	Muted = muted;
}
