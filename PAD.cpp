#include "PAD.h"

namespace PAD {
	const char* VersionString() { return "0.0.0"; }

	std::vector<AudioDevice*> __devices;
	void __RegisterDevice(AudioDevice *d) { __devices.push_back(d); }

	AudioDeviceCollection AudioDevice::Enumerate() 
	{
		return AudioDeviceCollection(__devices.data(),__devices.data()+__devices.size());
	}

	AudioStreamConfiguration::AudioStreamConfiguration(double samplerate):sampleRate(samplerate)
	{
	}
}