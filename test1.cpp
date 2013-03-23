#include <iostream>
#include <iomanip>
#define _USE_MATH_DEFINES
#include <math.h>
#include "PAD.h"

int main()
{
	using namespace PAD;
	Session myAudioSession;
	for(auto& dev : myAudioSession)
	{
		std::cout << dev << "\n  * Stereo : " << dev.DefaultStereo() 
						 << "\n  * All    : " << dev.DefaultAllChannels() << "\n\n";
	}

	auto rme(myAudioSession("ASIO Fireface"));
	if (rme)
	{
		double phase = 0;
		auto audioProcess = Closure(([&](uint64_t time, const PAD::AudioStreamConfiguration&, const float *input, float *output, unsigned frames)
		{
			for(unsigned i(0);i<frames;++i)
			{
				output[i] = sin(phase);
				phase = phase + 0.01 * M_PI;
			}
		}));

		rme->Open(SampleRate(44100) + Outputs(0,4),audioProcess);
		getchar();
		rme->Close();
	}

}