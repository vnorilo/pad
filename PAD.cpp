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

	enum RangeFindResult {
		In,
		Below,
		Between,
		Above
	};

	bool ChannelRange::Overlaps(ChannelRange r)
	{
		return r.begin() < end() && r.end() > begin();
	}

	static void AddChannels(ChannelRange channels, vector<ChannelRange>& ranges)
	{
		/* check for valid range */
		for(auto r : ranges)
		{
			if (r.Overlaps(channels)) throw SoftError(ChannelRangeOverlap,"Channel ranges must not overlap");
		}

		/* merge into another range if continuous */
		for(auto& r : ranges)
		{
			if (channels.end() == r.begin())
			{
				r = ChannelRange(channels.begin(),r.end());
				return;
			}
		}

		ranges.push_back(channels);
	}

	static RangeFindResult IsChannelInRangeSet(unsigned idx, const vector<ChannelRange>& ranges)
	{
		bool aboveAllRanges(true);
		bool belowAllRanges(true);
		for(auto p : ranges)
		{
			if (idx < p.end())
			{
				aboveAllRanges = false;
				if (idx >= p.begin()) return In;
			}
			else if (idx >= p.begin()) belowAllRanges = false;			
		}

		if (aboveAllRanges) return Above;
		else if (belowAllRanges) return Below;
		else return Between;
	}

	static unsigned GetNumChannels(const vector<ChannelRange>& ranges)
	{
		unsigned idx(0);
		unsigned count(0);

		RangeFindResult r;
		while((r = IsChannelInRangeSet(idx,ranges)) != Above)
		{
			if (r == In) count++;
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

	bool AudioStreamConfiguration::IsInputEnabled(unsigned ch) const
	{
		return IsChannelInRangeSet(ch,inputRanges) == In;
	}

	bool AudioStreamConfiguration::IsOutputEnabled(unsigned ch) const
	{
		return IsChannelInRangeSet(ch,outputRanges) == In;
	}

	void AudioStreamConfiguration::AddInputs(ChannelRange channels)
	{
		AddChannels(channels,inputRanges);
	}

	void AudioStreamConfiguration::AddOutputs(ChannelRange channels)
	{
		AddChannels(channels,outputRanges);
	}
}