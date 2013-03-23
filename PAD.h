#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>
#include <iosfwd>

namespace PAD{
    const char* VersionString();

	enum ErrorCode {
		NoError,
		InternalError,
		ChannelRangeInvalid,
		ChannelRangeOverlap,
		UnknownApiIdentifier,
		DeviceStartStreamFailure,
		DeviceOpenStreamFailure,
		DeviceInitializationFailure,
		DeviceDriverFailure,
		DeviceDeinitializationFailure,
		DeviceCloseStreamFailure,
		DeviceStopStreamFailure
	};

	class Error : public std::runtime_error {
		ErrorCode code;
	protected:
		Error(ErrorCode c,const std::string& message):code(c),runtime_error(message.c_str()) {}
	public:
		ErrorCode GetCode() const {return code;}
	};

	/**
	* throw only SoftError and HardError 
	***/
	class SoftError : public Error {
	public:
		SoftError(ErrorCode c, const std::string& message):Error(c,message){}
	};

	class HardError : public Error {
	public:
		HardError(ErrorCode c, const std::string& message):Error(c,message){}
	};

	class ChannelRange{
		unsigned b, e;
	public:
		ChannelRange(unsigned b = 0, unsigned e = -1):b(b),e(e) {if(e<=b) throw SoftError(ChannelRangeInvalid,"Invalid channel range");}
		unsigned begin() const {return b;}
		unsigned end() const {return e;}
		bool Overlaps(ChannelRange);
		bool Contains(unsigned);
	};

	struct Channel : public ChannelRange{
		Channel(unsigned c):ChannelRange(c,c+1){}
	};

	class AudioStreamConfiguration {
		friend class AudioDevice;
		double sampleRate;
		std::vector<ChannelRange> inputRanges;
		std::vector<ChannelRange> outputRanges;
		unsigned bufferSize;
		bool valid;
	public:
		AudioStreamConfiguration(double sampleRate = 44100.0, bool valid = true);
		void SetSampleRate(double sampleRate) {this->sampleRate = sampleRate;}	

		void AddDeviceInputs(ChannelRange);
		void AddDeviceOutputs(ChannelRange);

		template <int N> void AddDeviceInputs(const ChannelRange (&ranges)[N]) {for(auto r : ranges) AddDeviceInputs(r);} 
		template <int N> void AddDeviceOutputs(const ChannelRange (&ranges)[N]) {for(auto r : ranges) AddDeviceOutputs(r);} 

		void SetPreferredBufferSize(unsigned frames) {bufferSize = frames;}

		bool IsInputEnabled(unsigned index) const;
		bool IsOutputEnabled(unsigned index) const;
		bool IsValid() const {return valid;}

		unsigned GetNumDeviceInputs() const;
		unsigned GetNumDeviceOutputs() const;

		unsigned GetNumStreamInputs() const;
		unsigned GetNumStreamOutputs() const;

		unsigned GetBufferSize() const {return bufferSize;}
		double GetSampleRate() const {return sampleRate;}	

		void SetValid(bool v) {valid = v;}
	};

    class AudioCallbackDelegate{
	public:
		/**
		 * Process is the audio callback. Expect it to be called from a realtime thread
		 */
        virtual void Process(uint64_t timestamp, const AudioStreamConfiguration&, const float* input, float *output, unsigned int frames) = 0;

		/**
		 * Utility setup calls that are called from the thread that controls the AudioDevice 
		 */
		virtual void AboutToBeginStream(const AudioStreamConfiguration&, AudioDevice*) {}
		virtual void AboutToEndStream(AudioDevice*) {}
		virtual void StreamDidEnd(AudioDevice*) {}
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

		virtual const AudioStreamConfiguration& Open(const AudioStreamConfiguration&, AudioCallbackDelegate&, bool startSuspended = false) = 0;


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
		AudioDevice* operator()(const char *match);

		void Register(AudioDevice*);

		AudioDevice& GetDefaultDevice(const char *onlyForApi = NULL);
	};
};

std::ostream& operator<<(std::ostream&, const PAD::AudioStreamConfiguration&);
std::ostream& operator<<(std::ostream&, const PAD::AudioDevice&);