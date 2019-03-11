#include "AsioUtil.h"

#include <string>
#include <set>
#include <list>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <memory>

#include "HostAPI.h"
#include "PAD.h"
#include "pad_samples.h"
#include "pad_samples_sse2.h"
#include "pad_channels.h"

#include "WinDebugStream.h"



static std::string utostr(unsigned code) {
	char buf[32];
	char *ptr = buf + 20;
	*ptr = 0;
	do {
		*--ptr = '0' + code % 10;
		code /= 10;
	} while (code);
	return std::string(ptr, buf + 32);
}

#define THROW_ERROR(code,expr) {ASIO::Error err = (expr); if (err != ASIO::OK) throw PAD::SoftError(code,std::string(#expr " failed with ASIO error ") + utostr(err));}
#define THROW_FALSE(code,expr) {if (expr == false) throw PAD::SoftError(code,#expr " failed");}
#define THROW_TRUE(code,expr) {if (expr == true) throw PAD::SoftError(code,#expr " failed");}

namespace {
	using namespace std;
	using namespace PAD;

	class AsioDevice;
	class AsioCallbackHolder {
		ASIO::Callbacks cb;
	public:
		void Allocate(AsioDevice&);
		void Release(AsioDevice&);
		operator ASIO::Callbacks*() { return &cb; }
		ASIO::Callbacks* operator->() { return &cb; }
	};

	class AsioDevice : public AudioDevice {
		enum AsioState {
			Idle,
			Loaded,
			Initialized,
			Prepared,
			Running
		} State = AsioState::Idle;

		double CPU_Load( ) const { return current_cpu_load; }
		
		ASIO::DriverRecord driverInfo;
		string deviceName;

		unsigned numInputs, numOutputs;
		unsigned index;

		AudioStreamConfiguration currentConfiguration;
		vector<ASIO::BufferInfo> bufferInfos;
		vector<ASIO::ChannelInfo> channelInfos;
		vector<float> delegateBufferInput, delegateBufferOutput;
		unsigned callbackBufferFrames, streamNumInputs, streamNumOutputs;
		std::chrono::microseconds inputLatency, outputLatency;

		AudioStreamConfiguration defaultMono, defaultStereo, defaultAll;
		AudioStreamConfiguration DefaultMono( ) const { return defaultMono; }
		AudioStreamConfiguration DefaultStereo( ) const { return defaultStereo; }
		AudioStreamConfiguration DefaultAllChannels( ) const { return defaultAll; }

		ASIO::IASIO& ASIO( ) {
			if (State < Loaded) {
				ASIO::Error err = driverInfo.driverObject->init(GetDesktopWindow());
				State = Loaded;
			}
			return *driverInfo.driverObject.Get();
		}

		void _AsioUnwind(AsioState to) {
			switch (State) {
			case Running:
				if (to >= Running) break;
				THROW_ERROR(DeviceStopStreamFailure, ASIO().stop());
				State = Prepared;
			case Prepared:
				if (to >= Prepared) break;
				THROW_ERROR(DeviceCloseStreamFailure, ASIO().disposeBuffers());
				State = Initialized;
			case Initialized:
				if (to >= Initialized) break;
				callbacks.Release(*this);
				State = Loaded;
			case Loaded:
				if (to >= Loaded) break;
				State = Idle;
			case Idle:
				if (to >= Idle) break;				
				break;
			}

			this_thread::sleep_for(chrono::milliseconds(20));
		}

		void AsioUnwind(AsioState to) {
			if (GetBufferSwitchLock( )) {
				lock_guard<recursive_mutex> lock(*GetBufferSwitchLock( ));
				_AsioUnwind(to);
			} else _AsioUnwind(to);
		}

	public:
		AsioDevice(ASIO::DriverRecord comDriverInfo, shared_ptr<recursive_mutex> callbackMtx, double defaultRate, const string& name, unsigned inputs, unsigned outputs) :
			deviceName(name), numInputs(inputs), numOutputs(outputs), driverInfo(comDriverInfo) {
			if (callbackMtx) SetBufferSwitchLock(std::move(callbackMtx));

			if (numOutputs >= 1) {
				defaultMono = AudioStreamConfiguration(defaultRate, true);
				defaultMono.AddDeviceOutputs(Channel(0));
				if (numInputs > 0) defaultMono.AddDeviceInputs(Channel(0));
			} else defaultMono.SetValid(false);

			if (numOutputs >= 2) {
				defaultStereo = AudioStreamConfiguration(defaultRate, true);
				defaultStereo.AddDeviceOutputs(ChannelRange(0, 2));
				if (numInputs > 0)
					defaultStereo.AddDeviceInputs(ChannelRange(0, min(numInputs, 2u)));
			} else defaultStereo.SetValid(false);
			defaultAll = AudioStreamConfiguration(defaultRate, true);
			defaultAll.AddDeviceOutputs(ChannelRange(0, numOutputs));
			defaultAll.AddDeviceInputs(ChannelRange(0, numInputs));
		}

		~AsioDevice( ) {
			AsioUnwind(Idle);
		}

		const char *GetName( ) const { return deviceName.c_str( ); }
		const char *GetHostAPI( ) const { return "ASIO"; }

		unsigned GetNumInputs( ) const { return numInputs; }
		unsigned GetNumOutputs( ) const { return numOutputs; }

		virtual bool Supports(const AudioStreamConfiguration& conf) const {
			if (conf.GetNumDeviceInputs( ) > GetNumInputs( ) ||
				conf.GetNumDeviceOutputs( ) > GetNumOutputs( )) return false;
			return false;
		}

		double current_cpu_load = 0.0;

		void BufferSwitch(long doubleBufferIndex, ASIO::Bool directProcess) {
			try {
				ASIO::Time time;
				memset(&time, 0, sizeof(ASIO::Time));
				time.timeInfo.flags = ASIO::SystemTimeValid | ASIO::SamplePositionValid | ASIO::SampleRateValid;
				time.timeInfo.sampleRate = currentConfiguration.GetSampleRate();
				ASIO().getSamplePosition(&time.timeInfo.samplePosition, &time.timeInfo.systemTime);
				BufferSwitchTimeInfo(&time, doubleBufferIndex, directProcess);
			} catch (...) {
				OutputDebugStringA("exception in ASIO callback");
			}
		}

		void SampleRateDidChange(ASIO::SampleRate sRate) {
			currentConfiguration.SetSampleRate(sRate);
			StreamConfigurationDidChange(AudioStreamConfiguration::SampleRateDidChange,
				currentConfiguration);
		}

		void UpdateLatencies() {
			ASIO::SampleRate sr = 44100;
			ASIO().getSampleRate(&sr);
			long inLatency = 0, outLatency = 0;
			ASIO().getLatencies(&inLatency, &outLatency);
			inputLatency  = std::chrono::microseconds(inLatency  * 1000'000ull / (std::int64_t)sr);
			outputLatency = std::chrono::microseconds(outLatency * 1000'000ull / (std::int64_t)sr);
		}

		static std::chrono::microseconds GetTime() {
			std::chrono::milliseconds msTime(timeGetTime());
			return msTime;
		}

		std::chrono::microseconds DeviceTimeNow() const {
			return GetTime();
		}

		GetDeviceTime GetDeviceTimeCallback() const {
			return GetTime;
		}

		long AsioMessage(long selector, long value, void* message, double* opt) {
			switch (selector) {
			case ASIO::SelectorSupported:
				if (value == ASIO::ResetRequest
					|| value == ASIO::EngineVersion
					|| value == ASIO::ResyncRequest
					|| value == ASIO::LatenciesChanged
					// the following three were added for ASIO 2.0, you don't necessarily have to support them
					|| value == ASIO::SupportsTimeInfo
					|| value == ASIO::SupportsTimeCode
					|| value == ASIO::SupportsInputMonitor)
					return 1L;
				break;
			case ASIO::EngineVersion:
				return 2L;
			case ASIO::ResetRequest:
				return 1L;
				break;
			case ASIO::LatenciesChanged:
				UpdateLatencies();
				break;
			case ASIO::ResyncRequest:
			case ASIO::SupportsTimeInfo:
			case ASIO::SupportsTimeCode:
				return 1L;
			}
			return 0L;
		}

		void Init( ) {
			if (State < Initialized) {
				this->ASIO( ); // trigger loading of the driver
				State = Initialized;
			}
		}


		AsioCallbackHolder callbacks;
		void Prepare( ) {
			if (State < Prepared) {
				long minBuf, maxBuf, prefBuf, bufGran;
				callbacks.Allocate(*this);
				THROW_ERROR(DeviceOpenStreamFailure, ASIO( ).getBufferSize(&minBuf, &maxBuf, &prefBuf, &bufGran));
				callbackBufferFrames = prefBuf;

				long numInputs, numOutputs;
				ASIO().getChannels(&numInputs, &numOutputs);

				bufferInfos.clear( );
				for (unsigned i(0); i < GetNumInputs( ); ++i) {
					if (currentConfiguration.IsInputEnabled(i)) {
						ASIO::BufferInfo buf = {ASIO::True, (long)i, {0, 0}};
						bufferInfos.push_back(buf);
					}
				}

				streamNumInputs = (unsigned)bufferInfos.size( );
				delegateBufferInput.resize(callbackBufferFrames * streamNumInputs);

				for (unsigned i(0); i < GetNumOutputs( ); ++i) {
					if (currentConfiguration.IsOutputEnabled(i)) {
						ASIO::BufferInfo buf = {ASIO::False, (long)i, {0, 0}};
						bufferInfos.push_back(buf);
					}
				}

				streamNumOutputs = (unsigned)bufferInfos.size( ) - streamNumInputs;
				delegateBufferOutput.resize(callbackBufferFrames * streamNumOutputs);

				channelInfos.clear();
				channelInfos.resize(bufferInfos.size());
				for (unsigned i(0); i < bufferInfos.size(); ++i) {
					channelInfos[i].channel = bufferInfos[i].channelNum;
					channelInfos[i].isInput = bufferInfos[i].isInput;
					THROW_ERROR(DeviceOpenStreamFailure, ASIO().getChannelInfo(&channelInfos[i]));
				}

				THROW_ERROR(DeviceOpenStreamFailure, ASIO( ).createBuffers(bufferInfos.data( ), (long)bufferInfos.size( ), callbackBufferFrames, callbacks));
				State = Prepared;
			}
		}

		void Run( ) {
			if (State < Running) {
				THROW_ERROR(DeviceOpenStreamFailure, ASIO( ).start( ));
				State = Running;
			}
		}

		enum Direction {
			Input,
			Output
		};

		template <Direction MODE>
		static void Format(ASIO::SampleType type, float* interleaved, void** blocks, unsigned frames, unsigned channels, unsigned stride) {
			switch (type) {
			case ASIO::Int16MSB:
			{
				typedef HostSample<int16_t, float, -(1 << 15), (1 << 15) - 1, 0, true> AsioSmp;
				if (MODE == Output) ChannelConverter<AsioSmp>::DeInterleave(interleaved, (AsioSmp**)blocks, frames, channels, stride);
				else ChannelConverter<AsioSmp>::Interleave(interleaved, (const AsioSmp**)blocks, frames, channels, stride);
				break;
			}
			case ASIO::Int32MSB:
			{
				typedef HostSample<int32_t, float, -(1 << 24), (1 << 23) - 1, 8, true> AsioSmp;
				if (MODE == Output) ChannelConverter<AsioSmp>::DeInterleave(interleaved, (AsioSmp**)blocks, frames, channels, stride);
				else ChannelConverter<AsioSmp>::Interleave(interleaved, (const AsioSmp**)blocks, frames, channels, stride);
				break;
			}
			case ASIO::Int32MSB16:
			{
				typedef HostSample<int32_t, float, -(1 << 15), (1 << 15) - 1, 0, true> AsioSmp;
				if (MODE == Output) ChannelConverter<AsioSmp>::DeInterleave(interleaved, (AsioSmp**)blocks, frames, channels, stride);
				else ChannelConverter<AsioSmp>::Interleave(interleaved, (const AsioSmp**)blocks, frames, channels, stride);
				break;
			}
			case ASIO::Int32MSB18:
			{
				typedef HostSample<int32_t, float, -(1 << 17), (1 << 17) - 1, 0, true> AsioSmp;
				if (MODE == Output) ChannelConverter<AsioSmp>::DeInterleave(interleaved, (AsioSmp**)blocks, frames, channels, stride);
				else ChannelConverter<AsioSmp>::Interleave(interleaved, (const AsioSmp**)blocks, frames, channels, stride);
				break;
			}
			case ASIO::Int32MSB20:
			{
				typedef HostSample<int32_t, float, -(1 << 19), (1 << 19) - 1, 0, true> AsioSmp;
				if (MODE == Output) ChannelConverter<AsioSmp>::DeInterleave(interleaved, (AsioSmp**)blocks, frames, channels, stride);
				else ChannelConverter<AsioSmp>::Interleave(interleaved, (const AsioSmp**)blocks, frames, channels, stride);
				break;
			}
			case ASIO::Int32MSB24:
			{
				typedef HostSample<int32_t, float, -(1 << 23), (1 << 23) - 1, 0, true> AsioSmp;
				if (MODE == Output) ChannelConverter<AsioSmp>::DeInterleave(interleaved, (AsioSmp**)blocks, frames, channels, stride);
				else ChannelConverter<AsioSmp>::Interleave(interleaved, (const AsioSmp**)blocks, frames, channels, stride);
				break;
			}
			case ASIO::Float32MSB:
			{
				typedef HostSample<float, float, -1, 1, 0, true> AsioSmp;
				if (MODE == Output) ChannelConverter<AsioSmp>::DeInterleave(interleaved, (AsioSmp**)blocks, frames, channels, stride);
				else ChannelConverter<AsioSmp>::Interleave(interleaved, (const AsioSmp**)blocks, frames, channels, stride);
				break;
			}
				//case ASIO::Float64MSB: 
				//	{
				//		typedef HostSample<double,float,-1,1,0,true> AsioSmp;
				//		ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
				//		break;
				//	}
			case ASIO::Int16LSB:
			{
				typedef HostSample<int16_t, float, -(1 << 15), (1 << 15) - 1, 0, false> AsioSmp;
				if (MODE == Output) ChannelConverter<AsioSmp>::DeInterleave(interleaved, (AsioSmp**)blocks, frames, channels, stride);
				else ChannelConverter<AsioSmp>::Interleave(interleaved, (const AsioSmp**)blocks, frames, channels, stride);
				break;
			}
			case ASIO::Int32LSB:
			{
				typedef HostSample<int32_t, float, -(1 << 23), (1 << 23) - 1, 8, false> AsioSmp;
				if (MODE == Output) ChannelConverter<AsioSmp>::DeInterleave(interleaved, (AsioSmp**)blocks, frames, channels, stride);
				else ChannelConverter<AsioSmp>::Interleave(interleaved, (const AsioSmp**)blocks, frames, channels, stride);
				break;
			}
			case ASIO::Int32LSB16:
			{
				typedef HostSample<int32_t, float, -(1 << 15), (1 << 15) - 1, 0, false> AsioSmp;
				if (MODE == Output) ChannelConverter<AsioSmp>::DeInterleave(interleaved, (AsioSmp**)blocks, frames, channels, stride);
				else ChannelConverter<AsioSmp>::Interleave(interleaved, (const AsioSmp**)blocks, frames, channels, stride);
				break;
			}
			case ASIO::Int32LSB18:
			{
				typedef HostSample<int32_t, float, -(1 << 17), (1 << 17) - 1, 0, false> AsioSmp;
				if (MODE == Output) ChannelConverter<AsioSmp>::DeInterleave(interleaved, (AsioSmp**)blocks, frames, channels, stride);
				else ChannelConverter<AsioSmp>::Interleave(interleaved, (const AsioSmp**)blocks, frames, channels, stride);
				break;
			}
			case ASIO::Int32LSB20:
			{
				typedef HostSample<int32_t, float, -(1 << 19), (1 << 19) - 1, 0, false> AsioSmp;
				if (MODE == Output) ChannelConverter<AsioSmp>::DeInterleave(interleaved, (AsioSmp**)blocks, frames, channels, stride);
				else ChannelConverter<AsioSmp>::Interleave(interleaved, (const AsioSmp**)blocks, frames, channels, stride);
				break;
			}
			case ASIO::Int32LSB24:
			{
				typedef HostSample<int32_t, float, -(1 << 23), (1 << 23) - 1, 0, false> AsioSmp;
				if (MODE == Output) ChannelConverter<AsioSmp>::DeInterleave(interleaved, (AsioSmp**)blocks, frames, channels, stride);
				else ChannelConverter<AsioSmp>::Interleave(interleaved, (const AsioSmp**)blocks, frames, channels, stride);
				break;
			}
			case ASIO::Float32LSB:
			{
				typedef HostSample<float, float, -1, 1, 0, false> AsioSmp;
				if (MODE == Output) ChannelConverter<AsioSmp>::DeInterleave(interleaved, (AsioSmp**)blocks, frames, channels, stride);
				else ChannelConverter<AsioSmp>::Interleave(interleaved, (const AsioSmp**)blocks, frames, channels, stride);
				break;
			}
				//case ASIO::Float64LSB: 
				//	{
				//		typedef HostSample<double,float,-1,1,0,false> AsioSmp;
				//		ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
				//		break;
				//	}
			default:break;
			}
		}

		ASIO::Time* BufferSwitchTimeInfo(ASIO::Time* params, long doubleBufferIndex, ASIO::Bool directProcess) {
			LARGE_INTEGER perf_freq, perf_t0, perf_t1;
			QueryPerformanceFrequency(&perf_freq);
			QueryPerformanceCounter(&perf_t0);
			ASIO::Time* result_ = nullptr;
			if (GetBufferSwitchLock( )) {
				lock_guard<recursive_mutex> lock(*GetBufferSwitchLock( ));
				result_ = _BufferSwitchTimeInfo(params, doubleBufferIndex, directProcess);
			} else result_ = _BufferSwitchTimeInfo(params, doubleBufferIndex, directProcess);
			QueryPerformanceCounter(&perf_t1);
			double elapsed_ms = double(perf_t1.QuadPart - perf_t0.QuadPart) / perf_freq.QuadPart * 1000.0;
			double callback_max_len = 1000.0 / this->currentConfiguration.GetSampleRate( )*callbackBufferFrames;
			current_cpu_load = 1.0 / callback_max_len*elapsed_ms;
			return result_;
		}

		ASIO::Time* _BufferSwitchTimeInfo(ASIO::Time* params, long doubleBufferIndex, ASIO::Bool directProcess) {
			/* convert ASIO format to canonical format */
			void *bufferPtr[64];
			unsigned block(0);
			ASIO::SampleType blockType(-1);
			unsigned strideInCanonicalBuffer = streamNumInputs;


			/* convert ASIO format to canonical format */
			if (streamNumInputs) {
				unsigned beg(0);
				blockType = channelInfos[beg].type;

				unsigned idx(1);
				while (idx < streamNumInputs) {
					assert(bufferInfos[idx].isInput && channelInfos[idx].isInput);
					if (channelInfos[idx].type != blockType || (idx - beg) >= 64) {
						for (unsigned j(beg); j != idx; ++j) bufferPtr[j - beg] = bufferInfos[j].buffers[doubleBufferIndex];
						Format<Input>(blockType, delegateBufferInput.data( ) + beg, (void**)bufferPtr, callbackBufferFrames, idx - beg, streamNumInputs);

						beg = idx;
						blockType = channelInfos[beg].type;
					}
					idx++;
				}

				if (beg < streamNumInputs) {
					for (unsigned j(beg); j != streamNumInputs; ++j) bufferPtr[j - beg] = bufferInfos[j].buffers[doubleBufferIndex];
					Format<Input>(blockType, delegateBufferInput.data( ) + beg, (void**)bufferPtr, callbackBufferFrames, streamNumInputs - beg, streamNumInputs);
				}
			}
			
			// system time is in nanosecs
			std::chrono::microseconds sysTime(params->timeInfo.systemTime / 1000);

			IO io{
				currentConfiguration,
				delegateBufferInput.data( ),
				delegateBufferOutput.data( ),
				callbackBufferFrames,
				sysTime - inputLatency,
				sysTime + outputLatency
			};

			AudioDevice::BufferSwitch(io);

			/* convert canonical format to ASIO format */
			if (streamNumOutputs) {
				unsigned streamNumChannels = streamNumInputs + streamNumOutputs;
				unsigned beg(streamNumInputs);
				blockType = channelInfos[beg].type;

				unsigned idx(beg + 1);
				while (idx < streamNumChannels) {
					assert(bufferInfos[idx].isInput == false);
					if (channelInfos[idx].type != blockType || (idx - beg) >= 64) {
						for (unsigned j(beg); j != idx; ++j) bufferPtr[j - beg] = bufferInfos[j].buffers[doubleBufferIndex];
						Format<Output>(blockType, delegateBufferOutput.data( ) + beg - streamNumInputs, bufferPtr, callbackBufferFrames, idx - beg, streamNumOutputs);

						beg = idx;
						blockType = channelInfos[beg].type;
					}
					idx++;
				}

				if (beg < streamNumChannels) {
					for (unsigned j(beg); j != streamNumChannels; ++j) bufferPtr[j - beg] = bufferInfos[j].buffers[doubleBufferIndex];
					Format<Output>(blockType, delegateBufferOutput.data( ) + beg - streamNumInputs, bufferPtr, callbackBufferFrames, streamNumChannels - beg, streamNumOutputs);
				}

				ASIO( ).outputReady( );
			}

			return params;
		}

		virtual const AudioStreamConfiguration& Open(const AudioStreamConfiguration& conf) {
			AsioUnwind(Initialized);

			ASIO::SampleRate sr(0);
			ASIO::Error err = ASIO( ).getSampleRate(&sr);

			if (sr != conf.GetSampleRate( )) ASIO( ).setSampleRate(conf.GetSampleRate( ));

			Init( );

			/* canonicalize passed format */
			currentConfiguration = conf;
			err = ASIO( ).getSampleRate(&sr);
			currentConfiguration.SetSampleRate(sr);
			currentConfiguration.SetDeviceChannelLimits(GetNumInputs( ), GetNumOutputs( ));

			Prepare( );

			currentConfiguration.SetBufferSize(callbackBufferFrames);
			UpdateLatencies();

			AboutToBeginStream(currentConfiguration);

			if (conf.HasSuspendOnStartup( ) == false) Run( );
			return currentConfiguration;
		}

		virtual void Resume( ) {
			if (State != Prepared) throw HardError(DeviceStartStreamFailure, "AsioDevice in incorrect state: Resume() must be preceded by a successful Open with startSuspended or Suspend() call");
			Run( );
		}

		virtual void Suspend( ) {
			AsioUnwind(Prepared);
			StreamDidEnd( );
		}

		virtual void Close( ) {
			bool didEnd(State == Running);
			AsioUnwind(Loaded);
			if (didEnd) StreamDidEnd( );
		}
	};

	struct AsioPublisher : public HostAPIPublisher {
		vector<unique_ptr<recursive_mutex>> deviceMutex;
		list<AsioDevice> devices;

		std::vector<ASIO::DriverRecord> AsioRecords;
		
		AsioPublisher() {
			CoInitialize(nullptr);
			AsioRecords = ASIO::GetDrivers();
		}

		~AsioPublisher( ) { 
			devices.clear( ); 
			AsioRecords.clear();
			CoUninitialize();
		}

		template <typename... ARGS>
		void RegisterDevice(Session& PADInstance, ARGS&&... args) {
			devices.emplace_back(std::forward<ARGS>(args)...);
			PADInstance.Register(&devices.back( ));
		}

		const char* GetName( ) const { return "ASIO"; }

		const std::thread::id mainThread = std::this_thread::get_id();

		void Publish(Session& PADInstance, DeviceErrorDelegate& handler) {
			const unsigned MaxNameLength = 64;
			static  set<string> BlackList;
			BlackList.insert("JackRouter");

/*			if (std::this_thread::get_id() != mainThread) {
				throw PAD::HardError(PAD::ErrorCode::DeviceDriverFailure, "ASIO drivers can not be initialized on secondary threads.");
			}*/

			for (auto drv : AsioRecords) {
				try {
					if (BlackList.find(drv.driverName) == BlackList.end( )) {
						long numInputs = 0;
						long numOutputs = 0;

						try {
							auto driver = drv.driverObject;
							if (driver.Get()) {
									ASIO::SampleRate currentSampleRate;

									THROW_FALSE(DeviceInitializationFailure, driver->init(GetDesktopWindow( )));
									THROW_ERROR(DeviceInitializationFailure, driver->getChannels(&numInputs, &numOutputs));
									THROW_ERROR(DeviceInitializationFailure, driver->getSampleRate(&currentSampleRate));

									RegisterDevice(PADInstance, drv, make_shared<recursive_mutex>( ), currentSampleRate, drv.driverName, numInputs, numOutputs);					
							}
						} catch (std::exception& e) {
							// device failed to open
							cwindbg() << "[ASIO] " << drv.driverName << " failed to initialize: " << e.what() << "\n";
						} 
					}
				} catch (SoftError s) {
					handler.Catch(s);
				} catch (HardError h) {
					handler.Catch(h);
				}
			}
		}

		void Cleanup(PAD::Session&) {
			devices.clear( );
		}
	} publisher;

	static ASIO::Time dummyTime;
	template <int IDX> class CallbackForwarder {
		static void bufferSwitch(long doubleBufferIndex, ASIO::Bool directProcess) {
			__try {
				AsioDevice* ad(GetDevice());
				if (ad) ad->BufferSwitch(doubleBufferIndex, directProcess);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				DWORD code = GetExceptionCode();
			}
		}

		static void sampleRateDidChange(ASIO::SampleRate sRate) {
			AsioDevice* ad(GetDevice( ));
			if (ad) ad->SampleRateDidChange(sRate);
		}

		static long asioMessage(long selector, long value, void* message, double* opt) {
			AsioDevice* ad(GetDevice( ));
			if (ad) return ad->AsioMessage(selector, value, message, opt); else return 0L;
		}

		static ASIO::Time* bufferSwitchTimeInfo(ASIO::Time* params, long doubleBufferIndex, ASIO::Bool directProcess) {
			AsioDevice* ad(GetDevice( ));
			if (ad) return ad->BufferSwitchTimeInfo(params, doubleBufferIndex, directProcess); else return &dummyTime;
		}

		static AsioDevice*& GetDevice( ) { static AsioDevice* device; return device; }
	public:
		static bool AlreadyHasCallbacks(AsioDevice* master) {
			return GetDevice( ) == master || CallbackForwarder<IDX - 1>::AlreadyHasCallbacks(master);
		}

		static ASIO::Callbacks GetCallbacks(AsioDevice* master) {
			if (GetDevice( )) return CallbackForwarder<IDX - 1>::GetCallbacks(master);
			else {
				GetDevice( ) = master;
				ASIO::Callbacks tmp = {bufferSwitch, sampleRateDidChange, asioMessage, bufferSwitchTimeInfo};
				return tmp;
			}
		}

		static void Free(AsioDevice* master) {
			if (GetDevice( ) == master) GetDevice( ) = 0;
			else CallbackForwarder<IDX - 1>::Free(master);
		}
	};

	template <> class CallbackForwarder<0> {
	public:
		static bool AlreadyHasCallbacks(AudioDevice* master) { return false; }
		static ASIO::Callbacks GetCallbacks(AsioDevice*) {
			throw SoftError(DeviceInitializationFailure, "Too many concurrent ASIO streams");
		}

		static void Free(AsioDevice*) {
			throw HardError(InternalError, "Error in AsioDevice callback allocation logic");
		}
	};

	static mutex callbackAllocationMutex;
	static const uint32_t NUM_CALLBACKS = 64;
	void AsioCallbackHolder::Allocate(AsioDevice& master) {
		lock_guard<mutex> guard(callbackAllocationMutex);
		if (CallbackForwarder<NUM_CALLBACKS>::AlreadyHasCallbacks(&master)) throw HardError(InternalError, "Error in AsioDevice callback allocation logic");
		cb = CallbackForwarder<NUM_CALLBACKS>::GetCallbacks(&master);
	}

	void AsioCallbackHolder::Release(AsioDevice& master) {
		lock_guard<mutex> guard(callbackAllocationMutex);
		CallbackForwarder<NUM_CALLBACKS>::Free(&master);
		memset(&cb, 0, sizeof(ASIO::Callbacks));
	}
}

namespace PAD {
	IHostAPI* LinkASIO( ) {
		return &publisher;
	}
}
