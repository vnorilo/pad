#include <vector>
#include <cstdint>

namespace PAD{
<<<<<<< local
	class AudioCallbackDelegate{
	public:
		virtual void Process(uint64_t timestamp, const float* input, float *output) = 0;
	};
=======
>>>>>>> other

<<<<<<< local
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
=======
	typedef void (*AudioCallbackFunction)(uint64_t timestamp, const float* input, float *output, void *user);
>>>>>>> other

<<<<<<< local
		bool IsInputEnabled(unsigned index) const;
		bool IsOutputEnabled(unsigned index) const;
		bool IsValid() const {return valid;}
=======
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
>>>>>>> other

<<<<<<< local
=======
		bool IsInputEnabled(unsigned index) const;
		bool IsOutputEnabled(unsigned index) const;
		bool IsValid() const {return valid;}

>>>>>>> other
		unsigned GetNumInputs();
		unsigned GetNumOutputs();

		unsigned GetBufferSize() const {return bufferSize;}
		double GetSampleRate() const {return sampleRate;}	
	};

	class AudioDevice;
	class AudioDeviceCollection {
		AudioDevice *b, *e;
	public:
		AudioDeviceCollection(AudioDevice *b, AudioDevice *e):b(b),e(e){}
		AudioDevice* begin() {return b;}
		AudioDevice* end() {return e;}
	};

	class AudioDevice {	
	public:
		virtual unsigned GetNumInputs() = 0;
		virtual unsigned GetNumOutputs() = 0;
		virtual const char *GetName() = 0;
		virtual const char *GetHostAPI() = 0;

		virtual bool Supports(const AudioStreamConfiguration&) = 0;

<<<<<<< local
		virtual const AudioStreamConfiguration& Open(AudioCallbackDelegate&, bool startSuspended = false) = 0;
=======
		virtual const AudioStreamConfiguration& Open(AudioCallbackFunction, bool startSuspended = false) = 0;
>>>>>>> other

		virtual void Resume() = 0;
		virtual void Suspend() = 0;

		virtual void Close() = 0;

		static AudioDeviceCollection Enumerate();
	};
};

