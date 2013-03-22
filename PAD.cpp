#include "PAD.h"
#include <functional>
#include <numeric>

namespace PAD {
	using namespace std;
	const char* VersionString() { return "0.0.0"; }

	vector<AudioDevice*> __devices;
	void __RegisterDevice(AudioDevice *d) { __devices.push_back(d); }

	AudioDeviceCollection AudioDevice::Enumerate() 
	{
		return AudioDeviceCollection(__devices.data(),__devices.data()+__devices.size());
	}

	AudioStreamConfiguration::AudioStreamConfiguration(double samplerate):sampleRate(samplerate)
	{
	}

	static unsigned GetNumChannels(const vector<pair<unsigned,unsigned>>& ranges)
	{
		unsigned idx(0);
		unsigned count(0);
		bool aboveAllRanges(false);
		while(aboveAllRanges == false)
		{
			aboveAllRanges = true;
			for(auto p : ranges)
			{
				if (idx < p.second)
				{
					aboveAllRanges = false;
					if (idx >= p.first) {++count;break;}
				}
			}
		}
		return count;
	}


	unsigned AudioStreamConfiguration::GetNumInputs() const
	{
		return GetNumChannels(inputRanges);
	}

	unsigned AudioStreamConfiguration::GetNumOutputs() const
	{
		return GetNumChannels(outputRanges);
	}
}