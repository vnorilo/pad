#include "PAD.h"

namespace PAD {
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