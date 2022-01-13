#include "pad.h"
#include <functional>
#include <numeric>
#include <ostream>
#include <algorithm>


namespace PAD {
	using namespace std;
	const char* VersionString( ) { return "1.1.0"; }

	AudioStreamConfiguration::AudioStreamConfiguration(double samplerate, bool valid)
		:sampleRate(samplerate), valid(valid), startSuspended(false), numStreamIns(0), numStreamOuts(0), bufferSize(512) { }

	enum RangeFindResult {
		In,
		Below,
		Between,
		Above
	};

	bool ChannelRange::Touches(ChannelRange r) {
		return r.begin( ) <= end( ) && r.end( ) >= begin( );
	}

	static RangeFindResult IsChannelInRangeSet(unsigned idx, const vector<ChannelRange>& ranges) {
		bool aboveAllRanges(true);
		bool belowAllRanges(true);
		for (auto p : ranges) {
			if (idx < p.end( )) {
				aboveAllRanges = false;
				if (idx >= p.begin( )) return In;
			} else if (idx >= p.begin( )) belowAllRanges = false;
		}

		if (aboveAllRanges) return Above;
		else if (belowAllRanges) return Below;
		else return Between;
	}

	static unsigned GetNumChannels(const vector<ChannelRange>& ranges) {
		unsigned idx(0);
		unsigned count(0);

		RangeFindResult r;
		while ((r = IsChannelInRangeSet(idx++, ranges)) != Above) {
			if (r == In) count++;
		}
		return count;
	}

	unsigned AudioStreamConfiguration::GetNumDeviceInputs( ) const {
		unsigned max(0);
		for (auto r : inputRanges) if (r.end( ) > max) max = r.end( );
		return max;
	}

	unsigned AudioStreamConfiguration::GetNumDeviceOutputs( ) const {
		unsigned max(0);
		for (auto r : outputRanges) if (r.end( ) > max) max = r.end( );
		return max;
	}

	void AudioStreamConfiguration::Normalize(std::vector<ChannelRange>& cr) {
		// put earliest, largest ranges first
		std::sort(cr.begin(), cr.end(), [](ChannelRange a, ChannelRange b) {
			if (a.begin() < b.begin()) return true;
			if (a.begin() > b.begin()) return false;
			return a.end() > b.end();
		});
		// combine overlapping ranges
		bool testAgain;
		do {
			testAgain = false;
			for (size_t i = 1;i < cr.size();++i) {
				if (cr[i - 1].Touches(cr[i])) {
					cr[i - 1] = {
						std::min(cr[i - 1].begin(), cr[i].begin()),
						std::max(cr[i - 1].end(), cr[i].end())
					};
					cr[i] = {};
					testAgain = true;
				}
			}
			// remove empty ranges
			auto last = std::remove_if(cr.begin(), cr.end(), [](ChannelRange cr) { return cr.begin() == cr.end(); });
			testAgain |= last != cr.end();
			cr.erase(last, cr.end());
		} while (testAgain);
	}

	bool AudioStreamConfiguration::IsInputEnabled(unsigned ch) const {
		return IsChannelInRangeSet(ch, inputRanges) == In;
	}

	bool AudioStreamConfiguration::IsOutputEnabled(unsigned ch) const {
		return IsChannelInRangeSet(ch, outputRanges) == In;
	}

	void AudioStreamConfiguration::AddDeviceInputs(ChannelRange channels) {
		inputRanges.emplace_back(channels);
		Normalize(inputRanges);
		numStreamIns = GetNumChannels(inputRanges);
	}

	void AudioStreamConfiguration::AddDeviceOutputs(ChannelRange channels) {
		outputRanges.emplace_back(channels);
		Normalize(outputRanges);
		numStreamOuts = GetNumChannels(outputRanges);
	}

    void AudioStreamConfiguration::SetInputRanges(std::initializer_list<ChannelRange> cr) {
        inputRanges = std::move(cr);
        Normalize(inputRanges);
        numStreamIns = GetNumChannels(inputRanges);
    }
    
    void AudioStreamConfiguration::SetOutputRanges(std::initializer_list<ChannelRange> cr) {
        outputRanges = std::move(cr);
        Normalize(outputRanges);
        numStreamOuts = GetNumChannels(outputRanges);
    }

    
	AudioStreamConfiguration AudioStreamConfiguration::SampleRate(double rate) const {
		auto tmp(*this); tmp.SetSampleRate(rate); return tmp;
	}

	AudioStreamConfiguration AudioStreamConfiguration::Input(unsigned ch) const {
		auto tmp(*this); tmp.AddDeviceInputs(ChannelRange(ch, ch + 1)); return tmp;
	}

	AudioStreamConfiguration AudioStreamConfiguration::Output(unsigned ch) const {
		auto tmp(*this); tmp.AddDeviceOutputs(ChannelRange(ch, ch + 1)); return tmp;
	}

	AudioStreamConfiguration AudioStreamConfiguration::StereoInput(unsigned index) const {
		auto tmp(*this); tmp.AddDeviceInputs(ChannelRange(index * 2, index * 2 + 2)); return tmp;
	}

	AudioStreamConfiguration AudioStreamConfiguration::StereoOutput(unsigned index) const {
		auto tmp(*this); tmp.AddDeviceOutputs(ChannelRange(index * 2, index * 2 + 2)); return tmp;
	}

	AudioStreamConfiguration AudioStreamConfiguration::Inputs(ChannelRange cr) const {
		auto tmp(*this); tmp.AddDeviceInputs(cr); return tmp;
	}

	AudioStreamConfiguration AudioStreamConfiguration::Outputs(ChannelRange cr) const {
		auto tmp(*this); tmp.AddDeviceOutputs(cr); return tmp;
	}
    
    AudioStreamConfiguration AudioStreamConfiguration::StartSuspended( ) const {
        auto tmp(*this); tmp.SetSuspendOnStartup(true); return tmp;
    }


	static void SetChannelLimits(vector<ChannelRange>& channelRanges, unsigned maxCh) {
		vector<ChannelRange> newChannelRange;
		for (auto cr : channelRanges) {
			if (cr.begin( ) < maxCh) {
				newChannelRange.push_back(ChannelRange(cr.begin( ), min(cr.end( ), maxCh)));
			}
		}

		channelRanges = newChannelRange;
	}

	void AudioStreamConfiguration::SetDeviceChannelLimits(unsigned maxIn, unsigned maxOut) {
		SetChannelLimits(inputRanges, maxIn);
		SetChannelLimits(outputRanges, maxOut);
		numStreamIns = GetNumChannels(inputRanges);
		numStreamOuts = GetNumChannels(outputRanges);
	}
}

namespace PAD {
	IHostAPI* LinkCoreAudio( );
	IHostAPI* LinkASIO( );
	IHostAPI* LinkWASAPI( );
	IHostAPI* LinkJACK( );

	std::vector<IHostAPI*> GetLinkedAPIs( ) {
		std::vector<IHostAPI*> hosts;
#ifdef PAD_LINK_ASIO
		hosts.push_back(LinkASIO( ));
#endif
#ifdef PAD_LINK_WASAPI
		hosts.push_back(LinkWASAPI( ));
#endif
#ifdef PAD_LINK_COREAUDIO
		hosts.push_back(LinkCoreAudio());
#endif
#ifdef PAD_LINK_JACK
		hosts.push_back(LinkJACK());
#endif

		return hosts;
	}

	ostream& operator<<(ostream& stream, const PAD::AudioDevice& dev) {
		stream << "[" << dev.GetHostAPI( ) << "] " << dev.GetName( ) << " [" << dev.GetNumInputs( ) << "x" << dev.GetNumOutputs( ) << "]";
		return stream;
	}

	ostream& operator<<(ostream& stream, const PAD::AudioStreamConfiguration& cfg) {
		if (cfg.IsValid( ) == false) {
			stream << "n/a";
			return stream;
		}

		stream << cfg.GetSampleRate( ) / 1000.0 << "kHz ";
		unsigned devIns(cfg.GetNumDeviceInputs( ));
		unsigned devOuts(cfg.GetNumDeviceOutputs( ));

		if (devIns < 32 && devOuts < 32 && (devIns > 0 || devOuts > 0)) {
			stream << "I/O [";
			for (unsigned i(0); i < devIns; ++i) {
				if (cfg.IsInputEnabled(i)) stream << "<" << i + 1 << ">";
			}

			stream << " x ";

			for (unsigned i(0); i < devOuts; ++i) {
				if (cfg.IsOutputEnabled(i)) stream << "<" << i + 1 << ">";
			}
			stream << "]";
		} else stream << "[" << devIns << "x" << devOuts << "]";

		return stream;
	}
}
