#pragma once
#include <vector>
#include <cstdint>

namespace PAD{
    static const char* versionString() { return "0.0.0"; }

    class AudioCallbackDelegate{
	public:
        virtual void Process(uint64_t timestamp, const float* input, float *output, unsigned int frames) = 0;
	};

	class AudioStreamConfiguration {
		friend class AudioDevice;
		double sampleRate;
		std::vector<std::pair<unsigned,unsigned>> inputRanges;
		std::vector<std::pair<unsigned,unsigned>> outputRanges;
		unsigned bufferSize;
		bool valid;
	public:
		AudioStreamConfiguration(double sampleRate = 44100.0);
		void SetSampleRate(double);	
		void AddInputs(unsigned first = 0, unsigned last = -1);
		void AddOutputs(unsigned first = 0, unsigned last = -1);
		void SetPreferredBufferSize(unsigned frames);

		bool IsInputEnabled(unsigned index) const;
		bool IsOutputEnabled(unsigned index) const;
		bool IsValid() const {return valid;}

		unsigned GetNumInputs();
		unsigned GetNumOutputs();

		unsigned GetBufferSize() const {return bufferSize;}
		double GetSampleRate() const {return sampleRate;}	
	};

	class AudioDevice;
	class AudioDeviceCollection {
		AudioDevice **b, **e;
	public:
		AudioDeviceCollection(AudioDevice **b, AudioDevice **e):b(b),e(e){}
		AudioDevice** begin() {return b;}
		AudioDevice** end() {return e;}
	};

	class AudioDevice {	
	public:
		virtual unsigned GetNumInputs() const = 0;
		virtual unsigned GetNumOutputs() const = 0;
		virtual const char *GetName() const = 0;
		virtual const char *GetHostAPI() const = 0;

		virtual bool Supports(const AudioStreamConfiguration&) const = 0;

		virtual const AudioStreamConfiguration& Open(AudioCallbackDelegate&, bool startSuspended = false) = 0;

		virtual void Resume() = 0;
		virtual void Suspend() = 0;

		virtual void Close() = 0;

		static AudioDeviceCollection Enumerate();
	};
};

