#define USE_AVX

#include <iostream>
#include <iomanip>
#define _USE_MATH_DEFINES
#include <math.h>
#include <algorithm>
#include <chrono>

#include "PAD.h"
#include "tvcomb.h"

int main() {

	vectored_comb<8> vc;
	float sum_l(0.f), sum_r(0.f);

	float triangle[] = { 0.5f, 0.5f, 1.f, 0.3f };

	for (int i(0); i < 8; ++i) {
		vc.set(i, 1.f, 1.f, i / 8.f, 10000.f * pow(2.f, i / 8.f), 0.8f, 0.5f, 0.001f, 0.f, 0.f, 100.f, 0.f, triangle, triangle);
	}

	auto start = std::chrono::high_resolution_clock::now();
	int num = 100000000;

	for (int i = 0; i < num; ++i) {
		float l, r;
		vc.tick(0.f, l, r);
		sum_l += l; sum_r += r;
	}

	auto dur = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - start);

	auto smp_s = (double)num / dur.count();
	std::cout << smp_s << " samples per second\n";
	std::cout << 100 * 44100.0 / smp_s << "% cpu @ 44.1\n";



	using namespace PAD;

	return 0;
	class ErrorLogger : public DeviceErrorDelegate {
	public:
		void Catch(SoftError e) { std::cerr << "*Soft " << e.GetCode() << "* :" << e.what() << "\n"; }
		void Catch(HardError e) { std::cerr << "*Hard " << e.GetCode() << "* :" << e.what() << "\n"; }
	};

    ErrorLogger log;	
	Session myAudioSession(true,&log);

	auto asioDevice = myAudioSession.FindDevice("audio codec");

	if (asioDevice != myAudioSession.end()) {
		try {

			asioDevice->BufferSwitch = [&](uint64_t time, const PAD::AudioStreamConfiguration& cfg, const float *input, float *output, unsigned frames) {
				unsigned numOuts(cfg.GetNumStreamOutputs());
				static double phase = 0;
				for (unsigned i(0); i < frames; ++i) {
					for (unsigned j(0); j < numOuts; ++j) {
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