#include <iostream>
#include <iomanip>
#include "PAD.h"
#include "pad_samples.h"
#include "pad_samples_sse2.h"

#include <numeric>

#include <chrono>

typedef PAD::Converter::HostSample<int32_t,float,0x80000000,0x7fffffff,false> Sample;

static const unsigned bufSize = 65536, channelCount = 16, repeats = 100;
Sample buffer[channelCount][bufSize];
Sample *blocks[channelCount];
float interleaved[bufSize*channelCount];

int main()
{
	PAD::Session myAudioSession;

	for(auto& dev : myAudioSession)
	{
		std::cout << dev << "\n  * Stereo : " << dev.DefaultStereo() 
						 << "\n  * All    : " << dev.DefaultAllChannels() << "\n\n";
	}


	for(unsigned i(0);i<channelCount;++i)
	{
		blocks[i] = buffer[i];
		for(unsigned j(0);j<bufSize;++j)
			blocks[i][j].data=int32_t(i * 0x1000000);
	}

	std::cout << "[Timing simple interleave/deinterleave/clip/round]\n";
	{
		auto start = std::chrono::high_resolution_clock::now();
		for(unsigned i(0);i<repeats;++i)
		{
			PAD::ChannelConverter<Sample>::InterleaveFallback(interleaved,(const Sample**)blocks,bufSize,channelCount,channelCount);
			PAD::ChannelConverter<Sample>::DeInterleaveFallback(interleaved,(Sample**)blocks,bufSize,channelCount,channelCount);
		}
		auto end = std::chrono::high_resolution_clock::now();
		auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
		std::cout << "Conversion of " << repeats * bufSize * channelCount << " samples: " << dur.count() << " ms\n";
		std::cout << (repeats * bufSize * channelCount) / dur.count() << " samples per millisecond\n";
		std::cout << std::setw(4) << 44100. * dur.count() / (repeats * bufSize * channelCount) << "% cpu utilization at 44.1kHz per channel\n"; 
	}

	std::cout << "\n[Timing vectored interleave/deinterleave/clip/round]\n";
	{
		auto start = std::chrono::high_resolution_clock::now();
		for(unsigned i(0);i<repeats;++i)
		{
			PAD::ChannelConverter<Sample>::InterleaveVectored(interleaved,(const Sample**)blocks,bufSize,channelCount,channelCount);
			PAD::ChannelConverter<Sample>::DeInterleaveVectored(interleaved,(Sample**)blocks,bufSize,channelCount,channelCount);
		}
		auto end = std::chrono::high_resolution_clock::now();
		auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
		std::cout << "Conversion of " << repeats * bufSize * channelCount << " samples: " << dur.count() << " ms\n";
		std::cout << (repeats * bufSize * channelCount) / dur.count() << " samples per millisecond\n";
		std::cout << std::setw(4) << 44100. * dur.count() / (repeats * bufSize * channelCount) << "% cpu utilization at 44.1kHz per channel\n"; 
	}
}