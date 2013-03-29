#pragma once
#include <vector>
#include <cstdint>
#include <iosfwd>

#include "PADErrors.h"

namespace PAD{
    const char* VersionString();

	class ChannelRange{
		unsigned b, e;
	public:
		
		ChannelRange(unsigned b = 0, unsigned e = -1):b(b),e(e) {if(e<b) throw SoftError(ChannelRangeInvalid,"Invalid channel range");}
		unsigned begin() const {return b;}
		unsigned end() const {return e;}
		bool Overlaps(ChannelRange);
		bool Contains(unsigned);
	};

	struct Channel : public ChannelRange{
		Channel(unsigned c):ChannelRange(c,c+1){}
	};

	class AudioCallbackDelegate;
	class AudioStreamConfiguration {
		friend class AudioDevice;
		double sampleRate;
		AudioCallbackDelegate* audioDelegate;
		std::vector<ChannelRange> inputRanges;
		std::vector<ChannelRange> outputRanges;
		unsigned numStreamIns;
		unsigned numStreamOuts;
		unsigned bufferSize;
		bool startSuspended;
		bool valid;
	public:
		AudioStreamConfiguration(double sampleRate = 44100.0, bool valid = true);
		void SetSampleRate(double sampleRate) {this->sampleRate = sampleRate;}	

		void SetValid(bool v) {valid = v;}

		void AddDeviceInputs(ChannelRange);
		void AddDeviceOutputs(ChannelRange);

		template <int N> void AddDeviceInputs(const ChannelRange (&ranges)[N]) {for(auto r : ranges) AddDeviceInputs(r);} 
		template <int N> void AddDeviceOutputs(const ChannelRange (&ranges)[N]) {for(auto r : ranges) AddDeviceOutputs(r);} 

		void SetBufferSize(unsigned frames) {bufferSize = frames;}

		void SetAudioDelegate(AudioCallbackDelegate& d) {audioDelegate = &d;}

		void SetSuspendOnStartup(bool suspend) {startSuspended = suspend;}

		bool IsInputEnabled(unsigned index) const;
		bool IsOutputEnabled(unsigned index) const;
	
		bool IsValid() const {return valid;}

		unsigned GetNumDeviceInputs() const;
		unsigned GetNumDeviceOutputs() const;

		unsigned GetNumStreamInputs() const {return numStreamIns;}
		unsigned GetNumStreamOutputs() const {return numStreamOuts;}

		unsigned GetBufferSize() const {return bufferSize;}
		double GetSampleRate() const {return sampleRate;}	

		bool HasAudioDelegate() const {return audioDelegate != NULL; }
		AudioCallbackDelegate& GetAudioDelegate() const {return *audioDelegate;}

		bool HasSuspendOnStartup() const {return startSuspended;}

		void SetDeviceChannelLimits(unsigned maximumDeviceInputChannel, unsigned maximumDeviceOutputChannel);

		/* Monad constructors for named parameter idion */
		AudioStreamConfiguration Input(unsigned ch) const;
		AudioStreamConfiguration Output(unsigned ch) const;
		AudioStreamConfiguration Inputs(ChannelRange) const;
		AudioStreamConfiguration Outputs(ChannelRange) const;
		AudioStreamConfiguration StereoInput(unsigned index) const;
		AudioStreamConfiguration StereoOutput(unsigned index) const;
		AudioStreamConfiguration Delegate(AudioCallbackDelegate& del) const;
		AudioStreamConfiguration SampleRate(double rate) const;
		AudioStreamConfiguration StartSuspended() const;
        
        const std::vector<ChannelRange> GetInputRanges() const {return inputRanges;}
        const std::vector<ChannelRange> GetOutputRanges() const {return outputRanges;}
	};

	static AudioStreamConfiguration Stream(AudioCallbackDelegate& d) {return AudioStreamConfiguration().Delegate(d);}

    class AudioDevice;
    class AudioCallbackDelegate{
	public:
		/**
		 * Process is the audio callback. Expect it to be called from a realtime thread
		 */
		virtual void Process(uint64_t timestamp, const AudioStreamConfiguration&, const float* input, float *output, unsigned int frames) = 0;

		/** 
		 * Stream callbacks that may occur from any thread
		 */
		enum ConfigurationChangeFlags{
			SampleRateDidChange = 0x0001,
			BufferSizeDidChange = 0x0002
		};

		virtual void StreamConfigurationDidChange(ConfigurationChangeFlags whatDidChange, const AudioStreamConfiguration& newConfig) {}

		/**
		 * Utility setup calls that are called from the thread that controls the AudioDevice 
		 */
		virtual void AboutToBeginStream(const AudioStreamConfiguration&, AudioDevice&) {}
		virtual void StreamDidEnd(AudioDevice&) {}
	};

	template <typename FUNCTOR> class AudioClosure : public AudioCallbackDelegate{
		FUNCTOR proc;
	public:
		AudioClosure(FUNCTOR p):proc(p){}
		virtual void Process(uint64_t ts, const AudioStreamConfiguration& conf, const float* in, float *out, unsigned int frames)
		{ proc(ts,conf,in,out,frames); }
	};

	template <typename FUNCTOR> AudioClosure<FUNCTOR> Closure(FUNCTOR f) { return AudioClosure<FUNCTOR>(f); }

	class AudioDevice {	
	public:
		virtual unsigned GetNumInputs() const = 0;
		virtual unsigned GetNumOutputs() const = 0;
		virtual const char *GetName() const = 0;
		virtual const char *GetHostAPI() const = 0;

		virtual bool Supports(const AudioStreamConfiguration&) const = 0;

		/**
		 * Streams that the non-discerning user would most likely want
		 ***/
		virtual AudioStreamConfiguration DefaultMono() const = 0;
		virtual AudioStreamConfiguration DefaultStereo() const = 0;
		virtual AudioStreamConfiguration DefaultAllChannels() const = 0;

		virtual const AudioStreamConfiguration& Open(const AudioStreamConfiguration&) = 0;

		virtual void Resume() = 0;
		virtual void Suspend() = 0;

		virtual void Close() = 0;
	};

	class HostAPIPublisher;

	class AudioDeviceIterator {
		friend class Session;
		AudioDevice** ptr;
		AudioDeviceIterator(AudioDevice** fromPtr):ptr(fromPtr) {}
	public:
		bool operator==(const AudioDeviceIterator& rhs) const {return ptr==rhs.ptr;}
		bool operator!=(const AudioDeviceIterator& rhs) const {return !(*this==rhs);}
		AudioDevice& operator*() {return **ptr;}
		AudioDevice* operator->() {return *ptr;}
		AudioDeviceIterator& operator++() {ptr++;return *this;}
		AudioDeviceIterator operator++(int) {auto tmp(*this);ptr++;return tmp;}
	};

	class DeviceErrorDelegate {
	public:
		virtual void Catch(SoftError) = 0;
		virtual void Catch(HardError) = 0;
	};

	class Session {
		std::vector<AudioDevice*> devices;
		std::vector<HostAPIPublisher*> heldAPIProviders;
		void InitializeApi(HostAPIPublisher*, DeviceErrorDelegate&);
	public:
		/* initialize all host apis */
		Session(bool loadAllAPIs = true, DeviceErrorDelegate* handler = NULL);
		~Session();

		std::vector<const char*> GetAvailableHostAPIs();
		void InitializeHostAPI(const char *hostApiName, DeviceErrorDelegate *del = 0);

		AudioDeviceIterator begin();
		AudioDeviceIterator end();
		AudioDeviceIterator FindDevice(const char *deviceNameRegexFilter);
		AudioDeviceIterator FindDevice(const char *hostApiRegexFilter, const char *deviceNameRegexFilter);

		void Register(AudioDevice*);

		AudioDevice& GetDefaultDevice(const char *onlyForApi = NULL);
	};
};

std::ostream& operator<<(std::ostream&, const PAD::AudioStreamConfiguration&);
std::ostream& operator<<(std::ostream&, const PAD::AudioDevice&);