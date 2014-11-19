
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

    ErrorLogger log;	
	Session myAudioSession(true,&log);

	for (auto& d : myAudioSession) {
		std::cout << d.GetHostAPI() << " " << d.GetName() << "\n";
	}

	auto asioDevice = myAudioSession.FindDevice(2);

	if (asioDevice != myAudioSession.end()) {
		try {
			asioDevice->BufferSwitch = [&](IO io) {
				unsigned numOuts(io.config.GetNumStreamOutputs());
				static double phase = 0;
				for (unsigned i(0); i < io.numFrames; ++i) {
					for (unsigned j(0); j < numOuts; ++j) {
						io.output[i*numOuts + j] = sin(phase);
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