#include <iostream>
#include <iomanip>
#define _USE_MATH_DEFINES
#include <math.h>
#include "PAD.h"

#include <xmmintrin.h>

class TripointOsc {
	float phase;
	float inc;
	float k1, k2, a;
	float sine;
public:
	TripointOsc():phase(0), inc(0) { Set(0.5f, 0.5f, 1.f, 0.f); }

	float ClampX(float x) {
		x = x < 0.00001f ? 0.00001f : x;
		x = x > 0.99999f ? 0.99999f : x;
		return x;
	}

	void Set(float x1, float x2, float x3, float sinusoidal) {
		x1 = ClampX(x1);
		x2 = ClampX(x2);
		x3 = ClampX(x3);
		x1 = x1 < 0.0001f ? 0.0001f : x1;
		k1 = 1.f / x1;

		k2 = -1.f / ClampX(x3 - x2);
		a = x2;

		sine = sinusoidal;
	}

	void SetFreq(float f, float sr) {
		inc = f / sr;
	}

	float Tick() {
		float s1 = phase * k1;
		s1 = s1 > 1.f ? 1.f : s1;

		float s2 = (phase - a) * k2;
		s2 = s2 > 0.f ? 0.f : s2;

		float x = s1 + s2;
		x = x < 0.f ? 0.f : x;
		
		phase += inc;
		phase -= phase >= 1.f ? 1.f : 0.f;

		float pseudo_sine = ((x * x / 2.f) - (x * x * x / 3.f)) * 6.f;

		float out = x + ((pseudo_sine - x) * sine);

		return out - 0.5f;
	}
};

int main() {
	using namespace PAD;

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

			TripointOsc lfo, osc;
			lfo.SetFreq(0.05, 44100);
			osc.SetFreq(110, 44100);


			asioDevice->BufferSwitch = [&](uint64_t time, const PAD::AudioStreamConfiguration& cfg, const float *input, float *output, unsigned frames) {
				unsigned numOuts(cfg.GetNumStreamOutputs());
				static double phase = 0;
				for (unsigned i(0); i < frames; ++i) {
					float l = lfo.Tick() + 0.5f;
					float out = osc.Tick() * l;
					for (unsigned j(0); j < numOuts; ++j) {
						output[i*numOuts + j] = out;
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