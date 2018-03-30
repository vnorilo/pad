#include "HostAPI.h"
#include "PAD.h"
#include "pad_samples.h"
#include "pad_samples_sse2.h"
#include "pad_channels.h"

#include "o2.h"

#include <memory>

namespace {
	using namespace std;
	using namespace PAD;

	static constexpr int NumChannels = 16;
	static constexpr int SampleRate = 44100;

	class O2Device : public AudioDevice {
	public:
		O2Device(const char *name) {

		}

		unsigned int GetNumInputs() const override {
			return NumChannels;
		}

		unsigned int GetNumOutputs() const override {
			return NumChannels;
		}

		const char *GetName() const override {
			return "Virtual Audio Device";
		}

		const char *GetHostAPI() const override {
			return "O2";
		}

		bool Supports(const PAD::AudioStreamConfiguration& cfg) const override {
			return cfg.GetNumDeviceInputs() <= GetNumInputs() && cfg.GetNumDeviceOutputs() <= GetNumOutputs();
		}

		AudioStreamConfiguration DefaultMono() const override {
			return AudioStreamConfiguration(SampleRate).Input(0).Output(0);
		}

		AudioStreamConfiguration DefaultStereo() const override {
			return AudioStreamConfiguration(SampleRate).Inputs({ 0, 2 }).Outputs({ 0, 2 });

		}

		AudioStreamConfiguration DefaultAllChannels() const override {
			return AudioStreamConfiguration(SampleRate).Inputs({ 0,GetNumInputs() }).Outputs({ 0, GetNumOutputs() });
		}

		const AudioStreamConfiguration& Open(const AudioStreamConfiguration& cfg) override {
			return cfg;
		}

		void Resume() override {

		}

		void Suspend() override {

		}

		void Close() override {

		}

		double CPU_Load() const override {
			return 0.0;
		}

		std::chrono::microseconds DeviceTimeNow() const override {
			return {};
		}
	};

	struct O2Publisher : public HostAPIPublisher {
		const char *GetName() const override {
			return "O2";
		}

		std::unique_ptr<O2Device> device;

		void Publish(Session& session, DeviceErrorDelegate& err) override {
			device = std::make_unique<O2Device>(PAD_O2_APPLICATION);
			session.Register(device.get());
		}
	} publisher;

}

namespace PAD {
	IHostAPI* LinkO2( ) {
		return &publisher;
	}
}
