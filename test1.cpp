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

    ErrorLogger log;	
	Session myAudioSession(true,&log);

	auto asioDevice = myAudioSession.FindDevice("wasapi", "idt");

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
	}
}