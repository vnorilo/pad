#include "AsioUtil.h"
#undef min
#undef max

#include <string>
#include <set>
#include <list>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <thread>

#include "HostAPI.h"
#include "PAD.h"
#include "pad_samples.h"
#include "pad_samples_sse2.h"
#include "pad_channels.h"

#include <iostream>

static std::string utostr(unsigned code)
{
	char buf[32];
	char *ptr = buf + 20;
	*ptr = 0;
	do {
		*--ptr = '0' + code % 10;
		code/=10;
	} 
	while(code);
	return std::string(ptr,buf+32);
}

#define THROW_ERROR(code,expr) {ASIO::Error err = (expr); if (err != ASIO::OK) throw PAD::SoftError(code,std::string(#expr " failed with ASIO error ") + utostr(err));}
#define THROW_FALSE(code,expr) {if (expr == false) throw PAD::SoftError(code,#expr " failed");}
#define THROW_TRUE(code,expr) {if (expr == true) throw PAD::SoftError(code,#expr " failed");}

namespace {
	using namespace std;
	using namespace PAD;

	class AsioDevice;
	class AsioCallbackHolder{
		ASIO::Callbacks cb;
	public:
		void Allocate(AsioDevice&);
		void Release(AsioDevice&);
		operator ASIO::Callbacks*() {return &cb;}
	};

	enum AsioState{
		Idle,
		Loaded,
		Initialized,
		Prepared,
		Running
	} State = Idle;

	class AsioDevice : public AudioDevice {

		ASIO::ComRef<ASIO::IASIO> driver;
		string deviceName;

		unsigned numInputs, numOutputs;
		unsigned index;

		volatile unsigned callbackEntryCounter;
		
		AudioStreamConfiguration currentConfiguration;
		AudioCallbackDelegate* currentDelegate;
		vector<ASIO::BufferInfo> bufferInfos;
		vector<ASIO::ChannelInfo> channelInfos;
		vector<float> delegateBufferInput, delegateBufferOutput;
		unsigned callbackBufferFrames, streamNumInputs, streamNumOutputs;

		AudioStreamConfiguration defaultMono, defaultStereo, defaultAll;
		AudioStreamConfiguration DefaultMono() const { return defaultMono; }
		AudioStreamConfiguration DefaultStereo() const { return defaultStereo; }
		AudioStreamConfiguration DefaultAllChannels() const { return defaultAll; }

		ASIO::IASIO& ASIO() {return *driver;}

	void AsioUnwind(AsioState to)
	{
		switch(State)
		{
		case Running:
			if (to == Running) break;
			THROW_ERROR(DeviceStopStreamFailure,ASIO().stop());

			while(callbackEntryCounter>0)
			{
				fprintf(stderr,"*WARNING* ASIO driver allowed premature stop stream");
				this_thread::yield();
			}

		case Prepared:
			State = Prepared;
			if (to == Prepared) break;
			THROW_ERROR(DeviceCloseStreamFailure,ASIO().disposeBuffers());
		case Initialized:
			State = Initialized;
			callbacks.Release(*this);
			if (to == Initialized) break;
		case Loaded:
			State = Loaded;
			if (to == Loaded) break;
		case Idle: break;
		}
	}

	public:
		AsioDevice(ASIO::ComRef<ASIO::IASIO> comDriver,double defaultRate, const string& name, unsigned inputs, unsigned outputs):
			deviceName(name),numInputs(inputs),numOutputs(outputs),driver(move(comDriver)),callbackEntryCounter(0)
		{
			if (numOutputs >= 1)
			{
				defaultMono = AudioStreamConfiguration(defaultRate,true);
				defaultMono.AddDeviceOutputs(Channel(0));
				if (numInputs > 0) defaultMono.AddDeviceInputs(Channel(0));
			}
			else defaultMono.SetValid(false);

			if (numOutputs >= 2)
			{
				defaultStereo = AudioStreamConfiguration(defaultRate,true);
				defaultStereo.AddDeviceOutputs(ChannelRange(0,2));
				if (numInputs > 0)
					defaultStereo.AddDeviceInputs(ChannelRange(0,min(numInputs,2u)));
			}
			else defaultStereo.SetValid(false);

			defaultAll = AudioStreamConfiguration(defaultRate,true);
			defaultAll.AddDeviceOutputs(ChannelRange(0,numOutputs));
			defaultAll.AddDeviceInputs(ChannelRange(0,numInputs));
		}

		~AsioDevice()
		{
			AsioUnwind(Initialized);
		}

		const char *GetName() const { return deviceName.c_str(); }
		const char *GetHostAPI() const { return "ASIO"; }

		unsigned GetNumInputs() const {return numInputs;}
		unsigned GetNumOutputs() const {return numOutputs;}

		virtual bool Supports(const AudioStreamConfiguration& conf) const
		{
			if (conf.GetNumDeviceInputs() > GetNumInputs() ||
				conf.GetNumDeviceOutputs() > GetNumOutputs()) return false;
			return false;
		}


		void BufferSwitch(long doubleBufferIndex, ASIO::Bool directProcess)
		{
			ASIO::Time time;
			memset(&time,0,sizeof(ASIO::Time));
			time.timeInfo.flags = ASIO::SystemTimeValid | ASIO::SamplePositionValid | ASIO::SampleRateValid;
			time.timeInfo.sampleRate = currentConfiguration.GetSampleRate();
			BufferSwitchTimeInfo(&time,doubleBufferIndex,directProcess);
		}

		void SampleRateDidChange(ASIO::SampleRate sRate)
		{
			currentConfiguration.SetSampleRate(sRate);
			if (currentDelegate) 
				currentDelegate->StreamConfigurationDidChange(
					AudioCallbackDelegate::SampleRateDidChange,currentConfiguration);
		}

		long AsioMessage(long selector, long value, void* message, double* opt)
		{
			switch(selector)
			{
			case ASIO::SelectorSupported:
				if(value == ASIO::ResetRequest
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
			case ASIO::ResyncRequest:
			case ASIO::LatenciesChanged:
			case ASIO::SupportsTimeInfo:
			case ASIO::SupportsTimeCode:
				return 1L;
			}
			return 0L;
		}

		void Init()
		{
			if (State < Initialized)
			{
				THROW_FALSE(DeviceInitializationFailure,ASIO().init(GetDesktopWindow()));
				State = Initialized;
			}
		}

//		ASIO::Callbacks asioCb;

		AsioCallbackHolder callbacks;
		void Prepare()
		{
			if (State < Prepared)
			{
				long minBuf, maxBuf, prefBuf, bufGran;
				//ASIO::Callbacks tmp = {BufferSwitch,SampleRateDidChange,AsioMessage,BufferSwitchTimeInfo};
				//asioCb = tmp;
				callbacks.Allocate(*this);
				THROW_ERROR(DeviceOpenStreamFailure,ASIO().getBufferSize(&minBuf,&maxBuf,&prefBuf,&bufGran));
				callbackBufferFrames = prefBuf;

				bufferInfos.clear();
				for(unsigned i(0);i<GetNumInputs();++i)
				{
					if (currentConfiguration.IsInputEnabled(i))
					{
						ASIO::BufferInfo buf = {ASIO::True,i,{0,0}};
						bufferInfos.push_back(buf);
					}
				}

				streamNumInputs = bufferInfos.size();
				delegateBufferInput.resize(callbackBufferFrames * streamNumInputs);

				for(unsigned i(0);i<GetNumOutputs();++i)
				{
					if (currentConfiguration.IsOutputEnabled(i))
					{
						ASIO::BufferInfo buf = {ASIO::False,i,{0,0}};						
						bufferInfos.push_back(buf);
					}
				}

				streamNumOutputs = bufferInfos.size() - streamNumInputs;
				delegateBufferOutput.resize(callbackBufferFrames * streamNumOutputs);

				THROW_ERROR(DeviceOpenStreamFailure,ASIO().createBuffers(bufferInfos.data(),bufferInfos.size(),callbackBufferFrames,callbacks));

				channelInfos.clear();
				channelInfos.resize(bufferInfos.size());
				for(unsigned i(0);i<bufferInfos.size();++i)
				{
					channelInfos[i].channel = bufferInfos[i].channelNum;
					channelInfos[i].isInput = bufferInfos[i].isInput;
					THROW_ERROR(DeviceOpenStreamFailure,ASIO().getChannelInfo(&channelInfos[i]));
				}

				State = Prepared;
			}
		}

		void Run()
		{
			if (State < Running)
			{
				ASIO().start();
				State = Running;
			}
		}


		static void FormatOutput(ASIO::SampleType type, const float* interleaved, void** blocks, unsigned frames, unsigned channels, unsigned stride)
		{
			switch(type)
			{
			case ASIO::Int16MSB: 
				{
					typedef HostSample<int16_t,float,-(1<<15),(1<<15)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32MSB: 
				{
					typedef HostSample<int32_t,float,-(1<<24),(1<<23)-1,8,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32MSB16: 
				{
					typedef HostSample<int32_t,float,-(1<<15),(1<<15)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32MSB18: 
				{
					typedef HostSample<int32_t,float,-(1<<17),(1<<17)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32MSB20: 
				{
					typedef HostSample<int32_t,float,-(1<<19),(1<<19)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32MSB24: 
				{
					typedef HostSample<int32_t,float,-(1<<23),(1<<23)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Float32MSB: 
				{
					typedef HostSample<float,float,-1,1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
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
					typedef HostSample<int16_t,float,-(1<<15),(1<<15)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32LSB: 
				{
					typedef HostSample<int32_t,float,-(1<<23),(1<<23)-1,8,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32LSB16: 
				{
					typedef HostSample<int32_t,float,-(1<<15),(1<<15)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32LSB18: 
				{
					typedef HostSample<int32_t,float,-(1<<17),(1<<17)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32LSB20: 
				{
					typedef HostSample<int32_t,float,-(1<<19),(1<<19)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32LSB24: 
				{
					typedef HostSample<int32_t,float,-(1<<23),(1<<23)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Float32LSB: 
				{
					typedef HostSample<float,float,-1,1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
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

		static void FormatInput(ASIO::SampleType type, float* interleaved, const void** blocks, unsigned frames, unsigned channels, unsigned stride)
		{
			switch(type)
			{
			case ASIO::Int16MSB: 
				{
					typedef HostSample<int16_t,float,-(1<<15),(1<<15)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32MSB: 
				{
					typedef HostSample<int32_t,float,-(1<<24),(1<<23)-1,8,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32MSB16: 
				{
					typedef HostSample<int32_t,float,-(1<<15),(1<<15)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32MSB18: 
				{
					typedef HostSample<int32_t,float,-(1<<17),(1<<17)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32MSB20: 
				{
					typedef HostSample<int32_t,float,-(1<<19),(1<<19)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32MSB24: 
				{
					typedef HostSample<int32_t,float,-(1<<23),(1<<23)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Float32MSB: 
				{
					typedef HostSample<float,float,-1,1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			//case ASIO::Float64MSB: 
			//	{
			//		typedef HostSample<double,float,-1,1,0,true> AsioSmp;
			//		ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
			//		break;
			//	}
			case ASIO::Int16LSB: 
				{
					typedef HostSample<int16_t,float,-(1<<16),(1<<16)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32LSB: 
				{
					typedef HostSample<int32_t,float,-(1<<23),(1<<23)-1,8,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32LSB16: 
				{
					typedef HostSample<int32_t,float,-(1<<15),(1<<15)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32LSB18: 
				{
					typedef HostSample<int32_t,float,-(1<<17),(1<<17)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32LSB20: 
				{
					typedef HostSample<int32_t,float,-(1<<19),(1<<19)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Int32LSB24: 
				{
					typedef HostSample<int32_t,float,-(1<<23),(1<<23)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIO::Float32LSB: 
				{
					typedef HostSample<float,float,-1,1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			//case ASIO::Float64LSB: 
			//	{
			//		typedef HostSample<double,float,-1,1,0,false> AsioSmp;
			//		ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
			//		break;
			//	}
			default:break;
			}
		}


		ASIO::Time* BufferSwitchTimeInfo(ASIO::Time* params, long doubleBufferIndex, ASIO::Bool directProcess)
		{
			callbackEntryCounter++;

			/* convert ASIO format to canonical format */
			void *bufferPtr[64];
			unsigned block(0);
			ASIO::SampleType blockType(-1);
			unsigned strideInCanonicalBuffer = streamNumInputs;
						

			/* convert ASIO format to canonical format */
			if (streamNumInputs)
			{
				unsigned beg(0);
				blockType = channelInfos[beg].type;

				unsigned idx(1);
				while(idx<streamNumInputs)
				{
					assert(bufferInfos[idx].isInput && channelInfos[idx].isInput );
					if (channelInfos[idx].type != blockType || (idx - beg) >= 64)
					{
						for(unsigned j(beg);j!=idx;++j) bufferPtr[j-beg] = bufferInfos[j].buffers[doubleBufferIndex];
						FormatInput(blockType,delegateBufferInput.data()+beg,(const void**)bufferPtr,callbackBufferFrames,idx-beg,streamNumInputs);

						beg = idx;
						blockType = channelInfos[beg].type;
					}
					idx++;
				}

				if (beg<streamNumInputs)
				{
					for(unsigned j(beg);j!=streamNumInputs;++j) bufferPtr[j-beg] = bufferInfos[j].buffers[doubleBufferIndex];
					FormatInput(blockType, delegateBufferInput.data()+beg,(const void**)bufferPtr,callbackBufferFrames,streamNumInputs-beg,streamNumInputs);
				}
			}

			currentDelegate->Process(0ll,currentConfiguration,
									 delegateBufferInput.data(),
									 delegateBufferOutput.data(),
									 callbackBufferFrames);

			/* convert canonical format to ASIO format */
			if (streamNumOutputs)
			{
				unsigned streamNumChannels = streamNumInputs + streamNumOutputs;
				unsigned beg(streamNumInputs);
				blockType = channelInfos[beg].type;

				unsigned idx(beg+1);
				while(idx<streamNumChannels)
				{
					assert(bufferInfos[idx].isInput == false);
					if (channelInfos[idx].type != blockType || (idx - beg) >= 64)
					{
						for(unsigned j(beg);j!=idx;++j) bufferPtr[j-beg] = bufferInfos[j].buffers[doubleBufferIndex];
						FormatOutput(blockType,delegateBufferOutput.data()+beg-streamNumInputs,bufferPtr,callbackBufferFrames,idx-beg,streamNumOutputs);

						beg = idx;
						blockType = channelInfos[beg].type;
					}
					idx++;
				}

				if (beg<streamNumChannels)
				{
					for(unsigned j(beg);j!=streamNumChannels;++j) bufferPtr[j-beg] = bufferInfos[j].buffers[doubleBufferIndex];
					FormatOutput(blockType, delegateBufferOutput.data()+beg-streamNumInputs,bufferPtr,callbackBufferFrames,streamNumChannels-beg,streamNumOutputs);
				}

				ASIO().outputReady();
			}

			--callbackEntryCounter;
            return params;
		}

		virtual const AudioStreamConfiguration& Open(const AudioStreamConfiguration& conf)
		{
			AsioUnwind(Initialized);

			ASIO::SampleRate sr(0);
			ASIO().getSampleRate(&sr);

			if (sr!=conf.GetSampleRate()) ASIO().setSampleRate(conf.GetSampleRate());

			//Load();
			Init();

			/* canonicalize passed format */
			currentDelegate = &conf.GetAudioDelegate();
			currentConfiguration = conf;
			ASIO().getSampleRate(&sr);
			currentConfiguration.SetSampleRate(sr);
			currentConfiguration.SetDeviceChannelLimits(GetNumInputs(),GetNumOutputs());

			Prepare();

			currentConfiguration.SetBufferSize(callbackBufferFrames);

			currentDelegate->AboutToBeginStream(currentConfiguration,*this);

			if (conf.HasSuspendOnStartup() == false) Run();
			return currentConfiguration;
		}

		virtual void Resume() 
		{
			if (State != Prepared) throw HardError(DeviceStartStreamFailure,"AsioDevice in incorrect state: Resume() must be preceded by a successful Open with startSuspended or Suspend() call");
			Run();
		}

		virtual void Suspend() 
		{
			AsioUnwind(Prepared);
			if (currentDelegate) currentDelegate->StreamDidEnd(*this);
		}

		virtual void Close() 
		{
			bool didEnd(State == Running);
			AsioUnwind(Loaded);
			if (didEnd && currentDelegate) currentDelegate->StreamDidEnd(*this);
		}
	};

	struct AsioPublisher : public HostAPIPublisher {		
		list<AsioDevice> devices;
		void RegisterDevice(Session& PADInstance, AsioDevice dev)
		{
			devices.push_back(dev);
			PADInstance.Register(&devices.back());
		}

		const char* GetName() const {return "ASIO";}

		void Publish(Session& PADInstance, DeviceErrorDelegate& handler)
		{
			const unsigned MaxNameLength = 64;
			static  set<string> BlackList;
			BlackList.insert("JackRouter");

			std::vector<ASIO::DriverRecord> drivers = ASIO::GetDrivers();

            for(auto drv : drivers)
			{
				try
				{
					if (BlackList.find(drv.driverName) == BlackList.end())
					{
						long numInputs = 0;
						long numOutputs = 0;

						ASIO::ComRef<ASIO::IASIO> driver = drv.Load();
						ASIO::SampleRate currentSampleRate;

						THROW_FALSE(DeviceInitializationFailure,driver->init(GetDesktopWindow()));
						THROW_ERROR(DeviceInitializationFailure,driver->getChannels(&numInputs,&numOutputs));
						THROW_ERROR(DeviceInitializationFailure,driver->getSampleRate(&currentSampleRate));

						RegisterDevice(PADInstance,AsioDevice(driver,currentSampleRate,drv.driverName,numInputs,numOutputs));
					}
				}
				catch(SoftError s)
				{
					handler.Catch(s);
				}
				catch(HardError h)
				{
					handler.Catch(h);
				}
			}
		}

		void Cleanup()
		{
			devices.clear();
		}

	} publisher;

	static ASIO::Time dummyTime;
	template <int IDX> class CallbackForwarder{
		static void bufferSwitch(long doubleBufferIndex, ASIO::Bool directProcess) 
		{
			AsioDevice* ad(GetDevice());
			if (ad) ad->BufferSwitch(doubleBufferIndex,directProcess);
		}

		static void sampleRateDidChange(ASIO::SampleRate sRate) 
		{
			AsioDevice* ad(GetDevice());
			if (ad) ad->SampleRateDidChange(sRate);
		}

		static long asioMessage(long selector, long value, void* message, double* opt) 
		{
			AsioDevice* ad(GetDevice());
			if (ad) return ad->AsioMessage(selector,value,message,opt); else return 0L;
		}

		static ASIO::Time* bufferSwitchTimeInfo(ASIO::Time* params, long doubleBufferIndex, ASIO::Bool directProcess) 
		{
			AsioDevice* ad(GetDevice());
			if (ad) return ad->BufferSwitchTimeInfo(params,doubleBufferIndex,directProcess); else return &dummyTime;
		}

		static AsioDevice*& GetDevice() {static AsioDevice* device; return device;}
	public:
		static bool AlreadyHasCallbacks(AsioDevice* master)
		{
			return GetDevice() == master || CallbackForwarder<IDX-1>::AlreadyHasCallbacks(master);
		}

		static ASIO::Callbacks GetCallbacks(AsioDevice* master)
		{
			if (GetDevice()) return CallbackForwarder<IDX-1>::GetCallbacks(master);
			else
			{
				GetDevice() = master;
				ASIO::Callbacks tmp = {bufferSwitch,sampleRateDidChange,asioMessage,bufferSwitchTimeInfo};
				return tmp;
			}
		}

		static void Free(AsioDevice* master)
		{
			if (GetDevice() == master) GetDevice() = 0;
			else CallbackForwarder<IDX-1>::Free(master);
		}
	};

	template <> class CallbackForwarder<0>{
	public:
		static bool AlreadyHasCallbacks(AudioDevice* master) {return false;}
		static ASIO::Callbacks GetCallbacks(AsioDevice*)
		{
			throw SoftError(DeviceInitializationFailure,"Too many concurrent ASIO streams");
		}

		static void Free(AsioDevice*)
		{
			throw HardError(InternalError,"Error in AsioDevice callback allocation logic");
		}
	};

	static mutex callbackMutex;
	static const uint32_t NUM_CALLBACKS = 64;
	void AsioCallbackHolder::Allocate(AsioDevice& master)
	{
		lock_guard<mutex> guard(callbackMutex);
		if (CallbackForwarder<NUM_CALLBACKS>::AlreadyHasCallbacks(&master)) throw HardError(InternalError,"Error in AsioDevice callback allocation logic");
		cb = CallbackForwarder<NUM_CALLBACKS>::GetCallbacks(&master);
	}

	void AsioCallbackHolder::Release(AsioDevice& master)
	{
		lock_guard<mutex> guard(callbackMutex);
		CallbackForwarder<NUM_CALLBACKS>::Free(&master);
		memset(&cb,0,sizeof(ASIO::Callbacks));
	}
}
