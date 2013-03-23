#include <iostream>
#include <iomanip>
#define _USE_MATH_DEFINES
#include <math.h>
#include "PAD.h"

int main()
{
	PAD::Session myAudioSession;

	for(auto& dev : myAudioSession)
	{
		std::cout << dev << "\n  * Stereo : " << dev.DefaultStereo() 
						 << "\n  * All    : " << dev.DefaultAllChannels() << "\n\n";
	}

	auto rme(myAudioSession("ASIO Fireface"));
	if (rme)
	{
		double phase = 0;
		auto audioProcess = PAD::Closure(([&](uint64_t time, const PAD::AudioStreamConfiguration&, const float *input, float *output, unsigned frames)
		{
			for(unsigned i(0);i<frames;++i)
			{
				output[i*2] = sin(phase);
				output[i*2+1] = sin(phase * 1.1);
				phase = phase + 0.01 * M_PI;
			}
		}));

		rme->Open(rme->DefaultStereo(),audioProcess);
		getchar();
		rme->Close();
	}

}