#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>

namespace PAD{
    const char* VersionString();

	enum ErrorCode {
		NoError,
		InternalError,
		ChannelRangeInvalid,
		ChannelRangeOverlap,
		UnknownApiIdentifier
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

	class AudioStreamConfiguration {
		friend class AudioDevice;
		double sampleRate;
		std::vector<ChannelRange> inputRanges;
		std::vector<ChannelRange> outputRanges;
		unsigned bufferSize;
		bool valid;
	public:
		AudioStreamConfiguration(double sampleRate = 44100.0);
		void SetSampleRate(double sampleRate) {this->sampleRate = sampleRate;}	

		void AddInputs(ChannelRange);
		void AddOutputs(ChannelRange);

		template <int N> void AddInputs(const ChannelRange (&ranges)[N]) {for(auto r : ranges) AddInputs(r);} 
		template <int N> void AddOutputs(const ChannelRange (&ranges)[N]) {for(auto r : ranges) AddOutputs(r);} 

		void SetPreferredBufferSize(unsigned frames) {bufferSize = frames;}

		bool IsInputEnabled(unsigned index) const;
		bool IsOutputEnabled(unsigned index) const;
		bool IsValid() const {return valid;}

		unsigned GetNumInputs() const;
		unsigned GetNumOutputs() const;

		unsigned GetBufferSize() const {return bufferSize;}
		double GetSampleRate() const {return sampleRate;}	
	};

    class AudioCallbackDelegate{
	public:
		/**
		 * Process is the audio callback. Expect it to be called from a realtime thread
		 */
        virtual void Process(uint64_t timestamp, const float* input, float *output, unsigned int frames) = 0;

		/**
		 * Utility setup calls that are called from the thread that controls the AudioDevice 
		 */
		virtual void AboutToBeginStream(const AudioStreamConfiguration&, AudioDevice*) {}
		virtual void AboutToEndStream(AudioDevice*) {}
		virtual void StreamDidEnd(AudioDevice*) {}
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

	class Session {
		std::vector<AudioDevice*> devices;
		std::vector<HostAPIPublisher*> heldAPIProviders;
		void InitializeApi(HostAPIPublisher*);
	public:
		/* initialize all host apis */
		Session(bool loadAllAPIs = true);
		~Session();

		std::vector<const char*> GetAvailableHostAPIs();
		void InitializeHostAPI(const char *hostApiName);

		AudioDeviceIterator begin();
		AudioDeviceIterator end();

		void Register(AudioDevice*);
	};
};

