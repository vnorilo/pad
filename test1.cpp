#include <iostream>
#include <iomanip>
#define _USE_MATH_DEFINES
#include <math.h>
#include "PAD.h"

int main() {
	using namespace PAD;

	class ErrorLogger : public DeviceErrorDelegate {
	public:
		void Catch(SoftError e) { std::cerr << "*Soft " << e.GetCode() << "* :" << e.what() << "\n"; }
		void Catch(HardError e) { std::cerr << "*Hard " << e.GetCode() << "* :" << e.what() << "\n"; }
	};
<<<<<<< local
=======
    

    ErrorLogger log;	
	Session myAudioSession(true,&log);
>>>>>>> other

	ErrorLogger log;
	Session myAudioSession(true, &log);

	for (auto& dev : myAudioSession) {
		std::cout << dev << "\n  * Stereo : " << dev.DefaultStereo()
			<< "\n  * All    : " << dev.DefaultAllChannels() << "\n\n";
	}

<<<<<<< local
	auto asioDevice = myAudioSession.FindDevice("wasapi", "idt");
=======
	auto asioDevice = myAudioSession.FindDevice("wasapi","idt");
>>>>>>> other

	if (asioDevice != myAudioSession.end()) {
		try {
			double phase1 = 0, phase2 = 0;

			asioDevice->AboutToBeginStream = [](AudioStreamConfiguration conf) {
				std::cout << "* Beginning stream: " << conf << "\n";
			};

			asioDevice->StreamConfigurationDidChange = [](AudioStreamConfiguration::ConfigurationChangeFlags, AudioStreamConfiguration conf) {
				std::cout << "* New configuration: " << conf << "\n";
			};

			asioDevice->StreamDidEnd = []() {
				std::cout << "* Stream stopped\n";
			};

			asioDevice->BufferSwitch = SilenceOutput;

			asioDevice->BufferSwitch += [&](uint64_t time, const PAD::AudioStreamConfiguration& cfg, const float *input, float *output, unsigned frames) {
				unsigned numOuts(cfg.GetNumStreamOutputs());
				for (unsigned i(0); i < frames; ++i) {
					for (unsigned j(0); j < numOuts; ++j) {
						output[i*numOuts+j] += (float)sin(phase1) * 0.1f;
						phase1 = phase1 + 0.01 * M_PI;
					}
				}
			};

<<<<<<< local
			asioDevice->BufferSwitch += [&](uint64_t time, const PAD::AudioStreamConfiguration& cfg, const float *input, float *output, unsigned frames) {
				unsigned numOuts(cfg.GetNumStreamOutputs());
				for (unsigned i(0); i < frames; ++i) {
					for (unsigned j(0); j < numOuts; ++j) {
						output[i*numOuts+j] += (float)sin(phase2) * 0.1f;
						phase2 = phase2 + 0.011 * M_PI;
					}
				}
			};

			asioDevice->Open(asioDevice->DefaultStereo());
			getchar();
			asioDevice->Close();
		}
		catch (std::runtime_error e) {
			std::cerr << e.what();
		}
=======
		std::cout << "Actual stream parameters: " <<
		asioDevice->Open(Stream(myAudioProcess).StereoOutput(0).StereoInput(0).SampleRate(48000));
		getchar();
//		asioDevice->Close();
        }
        catch(std::runtime_error e)
        {
            std::cerr << e.what();
        }
>>>>>>> other
	}
}