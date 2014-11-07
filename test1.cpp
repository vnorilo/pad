#include <Windows.h>
#include "WinDebugStream.h"

#define USE_AVX

#include <iostream>
#include <iomanip>
#define _USE_MATH_DEFINES
#include <math.h>
#include <algorithm>
#include <chrono>

#include "PAD.h"


int main() {
	using namespace PAD;

	class ErrorLogger : public DeviceErrorDelegate {
	public:
		void Catch(SoftError e) { std::cerr << "*Soft " << e.GetCode() << "* :" << e.what() << "\n"; }
		void Catch(HardError e) { std::cerr << "*Hard " << e.GetCode() << "* :" << e.what() << "\n"; }
	};

	cwindbg() << 4567 << "\n" << 'c';

    ErrorLogger log;	
	Session myAudioSession(true,&log);

	for (auto& d : myAudioSession) {
		std::cout << d.GetHostAPI() << " " << d.GetName() << "\n";
	}

	auto asioDevice = myAudioSession.FindDevice("audio codec");

	if (asioDevice != myAudioSession.end()) {
		try {

			asioDevice->BufferSwitch = [&](uint64_t time, const PAD::AudioStreamConfiguration& cfg, const float *input, float *output, unsigned frames) {
				unsigned numOuts(cfg.GetNumStreamOutputs());
				static double phase = 0;
				for (unsigned i(0); i < frames; ++i) {
					for (unsigned j(0); j < numOuts; ++j) {
						output[i*numOuts + j] = sin(phase);
					}
					phase+=0.1;
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