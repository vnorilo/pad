#include <iostream>
#include <iomanip>
#include <cmath>
#include "PAD.h"

int main()
{
	PAD::Session myAudioSession;

	for(auto& dev : myAudioSession)
	{
		std::cout << dev << "\n  * Stereo : " << dev.DefaultStereo() 
						 << "\n  * All    : " << dev.DefaultAllChannels() << "\n\n";

		double phase = 0;

		auto closure = PAD::Closure([&](uint64_t time, const PAD::AudioStreamConfiguration& conf, const float* in, float *out, unsigned frames)
			{
				for(unsigned i(0);i<frames;++i)
				{
					out[i*2] = sin(phase);
					out[i*2+1] = sin(phase*1.1);
					phase += 0.01 * 3.1415;
				}
			});

		dev.Open(dev.DefaultStereo(),closure);

		getchar();

		dev.Close();
	}

}