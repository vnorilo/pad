#include "PAD.h"
#include <functional>
#include <numeric>
#include <ostream>

namespace PAD {
	using namespace std;
	const char* VersionString() { return "0.0.0"; }

	AudioStreamConfiguration::AudioStreamConfiguration(double samplerate, bool valid)
		:sampleRate(samplerate),valid(valid),startSuspended(false),numStreamIns(0),numStreamOuts(0),audioDelegate(NULL)
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
		while((r = IsChannelInRangeSet(idx++,ranges)) != Above)
		{
			if (r == In) count++;
		}
		return count;
	}

	unsigned AudioStreamConfiguration::GetNumDeviceInputs() const
	{
		unsigned max(0);
		for(auto r : inputRanges) if (r.end() > max) max = r.end();
		return max;
	}

	unsigned AudioStreamConfiguration::GetNumDeviceOutputs() const
	{
		unsigned max(0);
		for(auto r : outputRanges) if (r.end() > max) max = r.end();
		return max;
	}

	bool AudioStreamConfiguration::IsInputEnabled(unsigned ch) const
	{
		return IsChannelInRangeSet(ch,inputRanges) == In;
	}

	bool AudioStreamConfiguration::IsOutputEnabled(unsigned ch) const
	{
		return IsChannelInRangeSet(ch,outputRanges) == In;
	}

	void AudioStreamConfiguration::AddDeviceInputs(ChannelRange channels)
	{
		AddChannels(channels,inputRanges);
		numStreamIns = GetNumChannels(inputRanges);
	}

	void AudioStreamConfiguration::AddDeviceOutputs(ChannelRange channels)
	{
		AddChannels(channels,outputRanges);
		numStreamOuts = GetNumChannels(outputRanges);
	}

	AudioStreamConfiguration AudioStreamConfiguration::SampleRate(double rate) const
	{
		auto tmp(*this);tmp.SetSampleRate(rate);return tmp;
	}

	AudioStreamConfiguration AudioStreamConfiguration::Input(unsigned ch) const
	{
		auto tmp(*this);tmp.AddDeviceInputs(ChannelRange(ch,ch+1));return tmp;
	}

	AudioStreamConfiguration AudioStreamConfiguration::Output(unsigned ch) const
	{
		auto tmp(*this);tmp.AddDeviceOutputs(ChannelRange(ch,ch+1));return tmp;
	}

	AudioStreamConfiguration AudioStreamConfiguration::StereoInput(unsigned index) const
	{
		auto tmp(*this);tmp.AddDeviceInputs(ChannelRange(index*2,index*2+2));return tmp;
	}

	AudioStreamConfiguration AudioStreamConfiguration::StereoOutput(unsigned index) const
	{
		auto tmp(*this);tmp.AddDeviceOutputs(ChannelRange(index*2,index*2+2));return tmp;
	}

	AudioStreamConfiguration AudioStreamConfiguration::Delegate(AudioCallbackDelegate& d) const
	{
		auto tmp(*this);tmp.SetAudioDelegate(d);return tmp;
	}

	AudioStreamConfiguration AudioStreamConfiguration::Inputs(ChannelRange cr) const
	{
		auto tmp(*this);tmp.AddDeviceInputs(cr);return tmp;
	}

	AudioStreamConfiguration AudioStreamConfiguration::Outputs(ChannelRange cr) const
	{
		auto tmp(*this);tmp.AddDeviceOutputs(cr);return tmp;
	}

	void AudioStreamConfiguration::SetDeviceChannelLimits(unsigned maxIn, unsigned maxOut)
	{
		for(auto i(inputRanges.begin());i!=inputRanges.end();)
		{
			if (i->begin() >= maxIn) inputRanges.erase(i++);
			else *i++ = ChannelRange(i->begin(),min(maxIn,i->end()));
		}

		for(auto i(outputRanges.begin());i!=outputRanges.end();)
		{
			if (i->begin() >= maxIn) outputRanges.erase(i++);
			else *i++ = ChannelRange(i->begin(),min(maxIn,i->end()));
		}
	}
}


std::ostream& operator<<(std::ostream& stream, const PAD::AudioDevice& dev)
{
	stream << "[" << dev.GetHostAPI() << "] " << dev.GetName() << " [" << dev.GetNumInputs() << "x"<<dev.GetNumOutputs()<<"]";
	return stream;
}

std::ostream& operator<<(std::ostream& stream, const PAD::AudioStreamConfiguration& cfg)
{
	if (cfg.IsValid() == false)
	{
		stream << "n/a";
		return stream;
	}

	stream << cfg.GetSampleRate() / 1000.0 << "kHz ";
	unsigned devIns(cfg.GetNumDeviceInputs());
	unsigned devOuts(cfg.GetNumDeviceOutputs());

	if (devIns < 32 && devOuts < 32 && (devIns > 0 || devOuts > 0))
	{
		stream << "Device[";
		for(unsigned i(0);i<devIns;++i)
		{
			if (cfg.IsInputEnabled(i)) stream << "<"<<i+1<<">";
		}

		stream << " x ";

		for(unsigned i(0);i<devOuts;++i)
		{
			if (cfg.IsOutputEnabled(i)) stream << "<"<<i+1<<">";
		}
		stream << "]";
	}
	else stream << "["<<devIns<<"x"<<devOuts<<"]";

	return stream;
}