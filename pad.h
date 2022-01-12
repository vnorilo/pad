#pragma once
#include <vector>
#include <forward_list>
#include <mutex>
#include <memory>
#include <cstdint>
#include <functional>
#include <cassert>
#include <chrono>

#include "pad_errors.h"

#define PAD_GUI_CONTROL_PANEL_SUPPORT 1

namespace PAD {
	const char* VersionString( );

	class IHostAPI {
	public:
		virtual const char *GetName( ) const = 0;
	};

	class ChannelRange {
		unsigned b, e;
	public:
		ChannelRange() :b(-1), e(-1) {}
		ChannelRange(unsigned c) :b(c), e(c + 1) {};
		ChannelRange(unsigned b, unsigned e) :b(b), e(e) { if (e < b) throw SoftError(ChannelRangeInvalid, "Invalid channel range"); }
		unsigned begin( ) const { return b; }
		unsigned end( ) const { return e; }
		bool Touches(ChannelRange);
	};

	struct Channel : public ChannelRange {
		Channel(unsigned c) :ChannelRange(c, c + 1) { }
	};

	class AudioStreamConfiguration {
		friend class AudioDevice;
		double sampleRate;
		std::vector<ChannelRange> inputRanges;
		std::vector<ChannelRange> outputRanges;
		unsigned numStreamIns;
		unsigned numStreamOuts;
		unsigned bufferSize;
		bool startSuspended;
		bool valid;
		static void Normalize(std::vector<ChannelRange>&);
	public:
		AudioStreamConfiguration(double sampleRate = 44100.0, bool valid = true);
		void SetSampleRate(double newRate) { sampleRate = newRate; }

		void SetValid(bool v) { valid = v; }

		void AddDeviceInputs(ChannelRange);
		void AddDeviceOutputs(ChannelRange);

		template <int N> void AddDeviceInputs(const ChannelRange(&ranges)[N]) { for (auto r : ranges) AddDeviceInputs(r); }
		template <int N> void AddDeviceOutputs(const ChannelRange(&ranges)[N]) { for (auto r : ranges) AddDeviceOutputs(r); }

		void SetBufferSize(unsigned frames) { bufferSize = frames; }

		void SetSuspendOnStartup(bool suspend) { startSuspended = suspend; }

		bool IsInputEnabled(unsigned index) const;
		bool IsOutputEnabled(unsigned index) const;

		bool IsValid( ) const { return valid; }

		unsigned GetNumDeviceInputs( ) const;
		unsigned GetNumDeviceOutputs( ) const;

		unsigned GetNumStreamInputs( ) const { return numStreamIns; }
		unsigned GetNumStreamOutputs( ) const { return numStreamOuts; }

		unsigned GetBufferSize( ) const { return bufferSize; }
		double GetSampleRate( ) const { return sampleRate; }

		bool HasSuspendOnStartup( ) const { return startSuspended; }

		void SetDeviceChannelLimits(unsigned maximumDeviceInputChannel, unsigned maximumDeviceOutputChannel);

		/* Monad constructors for named parameter idion */
		AudioStreamConfiguration Input(unsigned ch) const;
		AudioStreamConfiguration Output(unsigned ch) const;
		AudioStreamConfiguration Inputs(ChannelRange) const;
		AudioStreamConfiguration Outputs(ChannelRange) const;
		AudioStreamConfiguration StereoInput(unsigned index) const;
		AudioStreamConfiguration StereoOutput(unsigned index) const;
		AudioStreamConfiguration SampleRate(double rate) const;
		AudioStreamConfiguration StartSuspended( ) const;

		const std::vector<ChannelRange> GetInputRanges( ) const { return inputRanges; }
		const std::vector<ChannelRange> GetOutputRanges( ) const { return outputRanges; }

        void SetInputRanges(std::initializer_list<ChannelRange> cr);
        void SetOutputRanges(std::initializer_list<ChannelRange> cr);

		enum ConfigurationChangeFlags {
			SampleRateDidChange = 0x0001,
			BufferSizeDidChange = 0x0002
		};
	};

	class IEvent;

	class IEventSubscriber {
	public:
		virtual void RemoveEvent(IEvent*) = 0;
	};

	class IEvent {
	public:
		virtual void RemoveSubscriber(IEventSubscriber*) = 0;
	};

	template <typename... ARGS> class Event : public IEvent {
		friend class EventSubscriber;
		std::forward_list<std::pair<IEventSubscriber*, std::function<void(ARGS...)>>> handlers;
		void AddSubscriber(IEventSubscriber* sub, const std::function<void(ARGS...)>& func) {
			handlers.emplace_front(sub, func);
		}

		void RemoveSubscriber(IEventSubscriber *sub) {
			handlers.remove_if(
				[sub](const std::pair<IEventSubscriber*, std::function<void(ARGS...)>>& p) { return p.first == sub; });
		}

		void Clear() {
			for (auto& h : handlers) if (h.first) h.first->RemoveEvent(this);
		}
	public:
		~Event( ) {
			Clear();
		}


		void operator()(const ARGS&... args) {
			for (auto& h : handlers) h.second(args...);
		}

		Event& operator=(const std::function<void(ARGS...)>& handler) {
			handlers.clear( ); return *this += handler;
		}

		Event& operator+=(const std::function<void(ARGS...)>& handler) {
			handlers.emplace_front(nullptr, handler); return *this;
		}
	};

	class EventSubscriber : public IEventSubscriber {
		std::forward_list<IEvent*> subscriptions;
	public:
		~EventSubscriber( ) {
			for (auto s : subscriptions) s->RemoveSubscriber(this);
		}

		void RemoveEvent(IEvent *evt) { subscriptions.remove(evt); }

		void Reset() {
			subscriptions.clear();
		}

		template <typename FN, typename... ARGS> void When(Event<ARGS...>& evt, const FN& f) {
			evt.AddSubscriber(this, f);
			subscriptions.emplace_front(&evt);
		}
	};

	struct IO {
		const AudioStreamConfiguration& config;
		const float *input;
		float *output;
		unsigned numFrames;
		std::chrono::microseconds inputBufferTime, outputBufferTime;
	};
 
	class AudioDevice {
		std::shared_ptr<std::recursive_mutex> deviceMutex;
	public:
		using BufferSwitchHandler = std::function<void( )>;

		virtual ~AudioDevice( ) { }
		virtual unsigned GetNumInputs( ) const = 0;
		virtual unsigned GetNumOutputs( ) const = 0;
		virtual const char *GetName( ) const = 0;
		virtual const char *GetHostAPI( ) const = 0;

		virtual bool Supports(const AudioStreamConfiguration&) const = 0;

		/**
		 * Streams that the non-discerning user would most likely want
		 ***/
		virtual AudioStreamConfiguration DefaultMono( ) const = 0;
		virtual AudioStreamConfiguration DefaultStereo( ) const = 0;
		virtual AudioStreamConfiguration DefaultAllChannels( ) const = 0;

		virtual const AudioStreamConfiguration& Open(const AudioStreamConfiguration&) = 0;

		virtual void Resume( ) = 0;
		virtual void Suspend( ) = 0;

		virtual void Close( ) = 0;

		virtual double CPU_Load( ) const = 0;
#if PAD_GUI_CONTROL_PANEL_SUPPORT
		void ShowControlPanel() 
		{ 
			if (ShowControlPanelFunc)
				ShowControlPanelFunc();
		}
		std::function<void(void)> ShowControlPanelFunc;
#endif

		std::shared_ptr<std::recursive_mutex>& GetBufferSwitchLock( ) { return deviceMutex; }
		void SetBufferSwitchLock(std::shared_ptr<std::recursive_mutex> lock) { deviceMutex = std::move(lock); }

		using GetDeviceTime = std::chrono::microseconds(*)();
		virtual GetDeviceTime GetDeviceTimeCallback() const = 0;
		virtual std::chrono::microseconds DeviceTimeNow() const = 0;

		Event<IO> BufferSwitch;
		Event<AudioStreamConfiguration> AboutToBeginStream;
		Event<> StreamDidEnd;
		Event<AudioStreamConfiguration::ConfigurationChangeFlags, AudioStreamConfiguration> StreamConfigurationDidChange;
	};

	static void SilenceOutput(uint64_t, const AudioStreamConfiguration& c, const float*, float *output, unsigned frames) {
		for (int i = 0, sz = c.GetNumStreamOutputs( ) * frames; i < sz; ++i) output[i] = 0.f;
	}

	class HostAPIPublisher;

	class AudioDeviceIterator {
		friend class Session;
		AudioDevice** ptr;
		AudioDeviceIterator(AudioDevice** fromPtr) :ptr(fromPtr) { }
	public:
		AudioDeviceIterator( ) :ptr(nullptr) { }
		bool operator==(const AudioDeviceIterator& rhs) const { return ptr == rhs.ptr; }
		bool operator!=(const AudioDeviceIterator& rhs) const { return !(*this == rhs); }
		AudioDevice& operator*() { assert(ptr); return **ptr; }

		AudioDevice* operator->() { assert(ptr); return *ptr; }
		operator AudioDevice*() { assert(ptr);  return *ptr; }
		AudioDeviceIterator& operator++() { ptr++; return *this; }
		AudioDeviceIterator operator++(int) { auto tmp(*this); ptr++; return tmp; }
	};

	class DeviceErrorDelegate {
	public:
		virtual void Catch(SoftError) = 0;
		virtual void Catch(HardError) = 0;
	};

	std::vector<IHostAPI*> GetLinkedAPIs( );

	static inline void* LinkAPIs( ) {
        static std::vector<IHostAPI*> apis = GetLinkedAPIs();
        return apis.data( );
        
    }

	class Session {
		std::vector<AudioDevice*> devices;
		std::vector<HostAPIPublisher*> heldAPIProviders;
		void InitializeApi(HostAPIPublisher*, DeviceErrorDelegate&);
	public:
		/* initialize all host apis */
		Session(bool loadAllAPIs = true, DeviceErrorDelegate* handler = NULL, void *forceLinkage = LinkAPIs( ));
		~Session( );

		std::vector<const char*> GetAvailableHostAPIs( );
		void InitializeHostAPI(const char *hostApiName, DeviceErrorDelegate *del = 0);

		AudioDeviceIterator begin( );
		AudioDeviceIterator end( );
		AudioDeviceIterator FindDevice(const char *deviceNameRegexFilter, int minNumOutputs = 0, int minNumInputs = 0);
		AudioDeviceIterator FindDevice(const char *hostApiRegexFilter, const char *deviceNameRegexFilter, int minNumOutputs = 0, int minNumInputs = 0);
		AudioDeviceIterator FindDevice(int minNumOutputs, int minNumInputs = 0);

		void Register(AudioDevice*);

		AudioDevice& GetDefaultDevice(const char *onlyForApi = NULL);
	};

	std::ostream& operator<<(std::ostream&, const PAD::AudioStreamConfiguration&);
	std::ostream& operator<<(std::ostream&, const PAD::AudioDevice&);
}
