// C++ standard libraries
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdlib.h>
#include <string>
#include <thread>
#include <utility>

// PulseAudio
#include <pulse/simple.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>

#include "Audio.hpp"
#include "Data.hpp"

class AudioSampler::AudioSamplerImpl {
public:
	std::atomic<bool> stopped;
	std::atomic<bool> modified;
	std::atomic<int> ups;

	AudioSamplerImpl(const AudioSettings& audioSettings) {
		init(audioSettings);

		audioThread = std::thread(&AudioSamplerImpl::run, std::ref(*this));
	}

	~AudioSamplerImpl() {
		stopped = true;
		audioThread.join();
		cleanup();
	}

	void copyData(AudioData& audioData) {
		audioMutexLock.lock();
			for (size_t i = 0; i < settings.bufferSize; ++i) {
				switch (settings.channels) {
					case 1:
						audioData.buffer[2*i  ] = ppAudioBuffer[i/settings.sampleSize][i%settings.sampleSize];
						audioData.buffer[2*i+1] = audioData.buffer[2*i];
						break;
					case 2:
						audioData.buffer[i] = ppAudioBuffer[i/settings.sampleSize][i%settings.sampleSize];
						break;
					default:
						this->stopped = true;
						break;
				}
			}
		audioMutexLock.unlock();

		modified = false;
	}

private:
	float** ppAudioBuffer;
	float* pSampleBuffer;

	std::mutex audioMutexLock;

	pa_simple* s;

	AudioSettings settings;

	std::thread audioThread;

	pa_mainloop* mainloop;

	int error;

	void init(const AudioSettings& audioSettings) {
		settings.channels   = audioSettings.channels;
		settings.sampleSize = audioSettings.sampleSize*audioSettings.channels;
		settings.bufferSize = audioSettings.bufferSize*audioSettings.channels;
		settings.sampleRate = audioSettings.sampleRate;
		settings.sinkName   = audioSettings.sinkName;

		stopped = false;
		modified = false;
		ups = 0;

		pSampleBuffer = new float[settings.sampleSize];

		ppAudioBuffer = new float*[settings.bufferSize/settings.sampleSize];

		for (uint32_t i = 0; i < settings.bufferSize/settings.sampleSize; ++i) {
			ppAudioBuffer[i] = new float[settings.sampleSize];
		}

		if (settings.sinkName.empty()) {
			getDefaultSink();
		}
		std::clog << "Using PulseAudio sink: \"" << settings.sinkName << "\"\n";
		setupPulse();
	}

	void run() {
		std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();
		int numUpdates = 0;

		while(!this->stopped) {
			if (pa_simple_read(s, reinterpret_cast<char*>(pSampleBuffer), sizeof(float)*settings.sampleSize, &error) < 0) {
				std::cerr << "pa_simple_read() failed: " << pa_strerror(error) << std::endl;
				this->stopped = true;
				break;
			}

			audioMutexLock.lock();
				for (size_t i = 0; i < settings.bufferSize/settings.sampleSize-1; ++i) {
					std::swap(ppAudioBuffer[i], ppAudioBuffer[i+1]);
				}
				std::swap(ppAudioBuffer[settings.bufferSize/settings.sampleSize-1], pSampleBuffer);
			audioMutexLock.unlock();
			this->modified = true;

			++numUpdates;
			std::chrono::steady_clock::time_point currentTime = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::seconds>(currentTime-lastFrame).count() >= 1) {
				ups = numUpdates;
				numUpdates = 0;
				lastFrame = currentTime;
			}
		}
	}

	void cleanup() {
		for (uint32_t i = 0; i < settings.bufferSize/settings.sampleSize; ++i) {
			delete[] ppAudioBuffer[i];
		}
		delete[] ppAudioBuffer;
		delete[] pSampleBuffer;

		pa_simple_free(s);
	}

	void getDefaultSink() {
		pa_mainloop_api* mainloopAPI;
		pa_context* context;

		mainloop = pa_mainloop_new();
		mainloopAPI = pa_mainloop_get_api(mainloop);
		context = pa_context_new(mainloopAPI, "Vkav");

		pa_context_connect(context, NULL, PA_CONTEXT_NOFLAGS, NULL);

		pa_context_set_state_callback(context, contextStateCallback, reinterpret_cast<void*>(this));

		pa_mainloop_run(mainloop, nullptr);

		pa_context_disconnect(context);
		pa_context_unref(context);

		pa_mainloop_free(mainloop);
	}

	void setupPulse() {
		pa_sample_spec ss = {};
		ss.format   = PA_SAMPLE_FLOAT32LE;
		ss.rate     = settings.sampleRate;
		ss.channels = settings.channels;

		pa_buffer_attr attr = {};
		attr.maxlength = (uint32_t)-1;
		attr.fragsize = settings.sampleSize;

		s = pa_simple_new(
		    	NULL,
		    	"Vkav",
		    	PA_STREAM_RECORD,
		    	settings.sinkName.c_str(),
		    	"recorder for Vkav",
		    	&ss,
		    	NULL,
		    	&attr,
		    	&error
		    );

		if (!s) {
			std::cerr << "pa_simple_new() failed:" << pa_strerror(error) << std::endl;
			stopped = true;
		}
	}

	static void callback(pa_context* c, const pa_server_info* i, void* userdata) {
		auto audio = reinterpret_cast<AudioSamplerImpl*>(userdata);
		audio->settings.sinkName = i->default_sink_name;
		audio->settings.sinkName += ".monitor";

		pa_mainloop_quit(audio->mainloop, 0);
	}

	static void contextStateCallback(pa_context* c, void* userdata) {
		auto audio = reinterpret_cast<AudioSamplerImpl*>(userdata);

		switch (pa_context_get_state(c)) {
			case PA_CONTEXT_READY:
				pa_operation_unref(pa_context_get_server_info(c, callback, userdata));
				break;
			case PA_CONTEXT_FAILED:
				pa_mainloop_quit(audio->mainloop, 0);
				break;
			case PA_CONTEXT_TERMINATED:
				pa_mainloop_quit(audio->mainloop, 0);
				break;
			default:
				// Do nothing
				break;
		}
	}
};

bool AudioSampler::stopped() {
	return audioSamplerImpl->stopped;
}

bool AudioSampler::modified() {
	return audioSamplerImpl->modified;
}

int AudioSampler::ups() {
	return audioSamplerImpl->ups;
}

void AudioSampler::start(const AudioSettings& audioSettings) {
	audioSamplerImpl = new AudioSamplerImpl(audioSettings);
}

void AudioSampler::stop() {
	delete audioSamplerImpl;
}

void AudioSampler::copyData(AudioData& audioData) {
	audioSamplerImpl->copyData(audioData);
}
