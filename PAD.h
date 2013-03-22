#pragma once
#include <vector>
#include <cstdint>
#include <stdexcept>

namespace PAD{
    const char* VersionString();

	enum ErrorCode {
		NoError,
		InternalError,
	};

	class Error : public std::runtime_error {
		ErrorCode code;
	public:
		Error(ErrorCode c,const std::string& message):code(c),runtime_error(message.c_str()) {}
		ErrorCode GetCode() const {return code;}
	};

	class SoftError : public Error {
	public:
		SoftError(ErrorCode c, const std::string& message):Error(c,message){}
	};

	class HardError : public Error {
	public:
		HardError(ErrorCode c, const std::string& message):Error(c,message){}
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
		void SetSampleRate(double sampleRate) {this->sampleRate = sampleRate;}	
		void AddInputs(unsigned first = 0, unsigned last = -1);
		void AddOutputs(unsigned first = 0, unsigned last = -1);
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

	class AudioDevice;
	class AudioDeviceIterator {
		friend class AudioDeviceCollection;
		AudioDevice** ptr;
		AudioDeviceIterator(AudioDevice** fromPtr):ptr(fromPtr) {}
	public:
		bool operator==(const AudioDeviceIterator& rhs) const {return ptr==rhs.ptr;}
		bool operator!=(const AudioDeviceIterator& rhs) const {return !(*this==rhs);}
		AudioDevice& operator*() {return **ptr;}
		AudioDeviceIterator& operator++() {ptr++;return *this;}
		AudioDeviceIterator operator++(int) {auto tmp(*this);ptr++;return tmp;}
	};
	class AudioDeviceCollection {
		AudioDevice **b, **e;
	public:
		AudioDeviceCollection(AudioDevice **b, AudioDevice **e):b(b),e(e){}
		AudioDeviceIterator begin() {return b;}
		AudioDeviceIterator end() {return e;}
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

	class Initializer {
	public:
		/* initialize all host apis */
		Initializer();
		void InitializeHostAPI(const char *hostApiName);
	};
};

