#include <iostream>
#include <iomanip>
#define _USE_MATH_DEFINES
#include <math.h>
#include "PAD.h"

int main()
{
	using namespace PAD;
	
	class ErrorLogger : public DeviceErrorDelegate {
	public:
		void Catch(SoftError e) {std::cerr << "*Soft "<<e.GetCode()<<"* :" << e.what() << "\n";}
		void Catch(HardError e) {std::cerr << "*Hard "<<e.GetCode()<<"* :" << e.what() << "\n";}
	};
	
	Session myAudioSession(true,&ErrorLogger());

	for(auto& dev : myAudioSession)
	{
		std::cout << dev << "\n  * Stereo : " << dev.DefaultStereo() 
						 << "\n  * All    : " << dev.DefaultAllChannels() << "\n\n";
	}

	auto asioDevice = myAudioSession.FindDevice("jack",".*");

	if (asioDevice != myAudioSession.end())
	{
		double phase = 0;
		auto myAudioProcess = Closure(([&](uint64_t time, const PAD::AudioStreamConfiguration& cfg, const float *input, float *output, unsigned frames)
		{
			unsigned numOuts(cfg.GetNumStreamOutputs());
			unsigned numIns(cfg.GetNumStreamInputs());
			for(unsigned i(0);i<frames;++i)
			{
				for(unsigned j(0);j<numOuts;++j)
				{
					output[i*numOuts+j] = (float)sin(phase * (1.0 + double(j)/numOuts));
					if (j<numIns) output[i*numOuts+j] = input[i*numIns+j];
					phase = phase + 0.01 * M_PI;
				}
			}
		}));

		std::cout << "Actual stream parameters: " <<
		asioDevice->Open(Stream().Outputs(ChannelRange(0,8)).Delegate(myAudioProcess));
		getchar();
		asioDevice->Close();
	}

}