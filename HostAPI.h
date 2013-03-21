#pragma once

#include <vector>

namespace PAD{
	class AudioDevice;
	void __RegisterDevice(AudioDevice*);
	extern std::vector<AudioDevice*> __devices;
};