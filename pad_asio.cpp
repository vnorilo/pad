#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"

#undef min
#undef max

#include <string>
#include <set>
#include <list>
#include <algorithm>
#include <unordered_map>

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

#define THROW_ERROR(code,expr) {ASIOError err = (expr); if (err != ASE_OK) throw PAD::SoftError(code,std::string(#expr " failed with ASIO error ") + utostr(err) + driverInfo.errorMessage );}
#define THROW_FALSE(code,expr) {if (expr == false) throw PAD::SoftError(code,#expr " failed");}
#define THROW_TRUE(code,expr) {if (expr == true) throw PAD::SoftError(code,#expr " failed");}

namespace {
	using namespace std;
	using namespace PAD;

	ASIODriverInfo driverInfo;
	
	AsioDrivers& GetDrivers()
	{
		static AsioDrivers drivers;
		return drivers;
	}

	enum AsioState{
		Idle,
		Loaded,
		Initialized,
		Prepared,
		Running
	} State = Idle;

	void AsioUnwind(AsioState to)
	{
		switch(State)
		{
		case Running:
			if (to == Running) break;
			THROW_ERROR(DeviceStopStreamFailure,ASIOStop());
		case Prepared:
			State = Prepared;
			if (to == Prepared) break;
			THROW_ERROR(DeviceCloseStreamFailure,ASIODisposeBuffers());
		case Initialized:
			State = Initialized;
			if (to == Initialized) break;
		case Loaded:
			State = Loaded;
			if (to == Loaded) break;
//			THROW_ERROR(DeviceDeinitializationFailure,ASIOExit());
			//drivers.removeCurrentDriver();
		case Idle: break;
		}
	}

	class AsioDevice : public AudioDevice {

		string deviceName;

		unsigned numInputs, numOutputs;
		unsigned index;
		
		AudioStreamConfiguration defaultMono, defaultStereo, defaultAll;
		AudioStreamConfiguration DefaultMono() const { return defaultMono; }
		AudioStreamConfiguration DefaultStereo() const { return defaultStereo; }
		AudioStreamConfiguration DefaultAllChannels() const { return defaultAll; }

	public:
		AsioDevice(unsigned i,double defaultRate, const string& name, unsigned inputs, unsigned outputs):
			deviceName(name),numInputs(inputs),numOutputs(outputs),index(i) 
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

			defaultAll = AudioStreamConfiguration(44100,true);
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

		static AudioStreamConfiguration currentConfiguration;
		static AudioCallbackDelegate* currentDelegate;
		static vector<ASIOBufferInfo> bufferInfos;
		static vector<ASIOChannelInfo> channelInfos;
		static vector<float> delegateBufferInput, delegateBufferOutput;
		static unsigned callbackBufferFrames, streamNumInputs, streamNumOutputs;

		static cdecl void BufferSwitch(long doubleBufferIndex, ASIOBool directProcess)
		{
			ASIOTime tmp;
			BufferSwitchTimeInfo(&tmp,doubleBufferIndex,directProcess);
		}

		static void SampleRateDidChange(ASIOSampleRate sRate)
		{
		}

		static long AsioMessage(long selector, long value, void* message, double* opt)
		{
			switch(selector)
			{
			case kAsioSelectorSupported:
				if(value == kAsioResetRequest
					|| value == kAsioEngineVersion
					|| value == kAsioResyncRequest
					|| value == kAsioLatenciesChanged
					// the following three were added for ASIO 2.0, you don't necessarily have to support them
					|| value == kAsioSupportsTimeInfo
					|| value == kAsioSupportsTimeCode
					|| value == kAsioSupportsInputMonitor)
					return 1L;
				break;
			case kAsioEngineVersion:
				return 2L;
			case kAsioResetRequest:
			case kAsioResyncRequest:
			case kAsioLatenciesChanged:
			case kAsioSupportsTimeInfo:
			case kAsioSupportsTimeCode:
				return 1L;
			}
			return 0L;
		}

		void Load()
		{
			if (State < Loaded)
			{
				std::vector<char> space(64);
				THROW_ERROR(DeviceDriverFailure,GetDrivers().asioGetDriverName(index,space.data(),space.size()));
				THROW_FALSE(DeviceDriverFailure,GetDrivers().loadDriver(space.data()));
				State = Loaded;
			}
		}

		void Init()
		{
			if (State < Initialized)
			{
				THROW_ERROR(DeviceInitializationFailure,ASIOInit(&driverInfo));
				State = Initialized;
			}
		}

		ASIOCallbacks asioCb;

		void Prepare()
		{
			if (State < Prepared)
			{
				long minBuf, maxBuf, prefBuf, bufGran;
				ASIOCallbacks tmp = {BufferSwitch,SampleRateDidChange,AsioMessage,BufferSwitchTimeInfo};
				asioCb = tmp;
				THROW_ERROR(DeviceOpenStreamFailure,ASIOGetBufferSize(&minBuf,&maxBuf,&prefBuf,&bufGran));
				callbackBufferFrames = prefBuf;

				bufferInfos.clear();
				for(unsigned i(0);i<GetNumInputs();++i)
				{
					if (currentConfiguration.IsInputEnabled(i))
					{
						ASIOBufferInfo buf = {ASIOTrue,i,{0,0}};
						bufferInfos.push_back(buf);
					}
				}

				streamNumInputs = bufferInfos.size();
				delegateBufferInput.resize(callbackBufferFrames * streamNumInputs);

				for(unsigned i(0);i<GetNumOutputs();++i)
				{
					if (currentConfiguration.IsOutputEnabled(i))
					{
						ASIOBufferInfo buf = {ASIOFalse,i,{0,0}};						
						bufferInfos.push_back(buf);
					}
				}

				streamNumOutputs = bufferInfos.size() - streamNumInputs;
				delegateBufferOutput.resize(callbackBufferFrames * streamNumOutputs);

				THROW_ERROR(DeviceOpenStreamFailure,ASIOCreateBuffers(bufferInfos.data(),bufferInfos.size(),callbackBufferFrames,&asioCb));

				channelInfos.clear();
				channelInfos.resize(bufferInfos.size());
				for(unsigned i(0);i<bufferInfos.size();++i)
				{
					channelInfos[i].channel = bufferInfos[i].channelNum;
					channelInfos[i].isInput = bufferInfos[i].isInput;
					THROW_ERROR(DeviceOpenStreamFailure,ASIOGetChannelInfo(&channelInfos[i]));
				}

				State = Prepared;
			}
		}

		void Run()
		{
			if (State < Running)
			{
				ASIOStart();
				State = Running;
			}
		}


		static void FormatOutput(ASIOSampleType type, const float* interleaved, void** blocks, unsigned frames, unsigned channels, unsigned stride)
		{
			switch(type)
			{
			case ASIOSTInt16MSB: 
				{
					typedef HostSample<int16_t,float,-(1<<15),(1<<15)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32MSB: 
				{
					typedef HostSample<int32_t,float,-(1<<24),(1<<23)-1,8,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32MSB16: 
				{
					typedef HostSample<int32_t,float,-(1<<15),(1<<15)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32MSB18: 
				{
					typedef HostSample<int32_t,float,-(1<<17),(1<<17)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32MSB20: 
				{
					typedef HostSample<int32_t,float,-(1<<19),(1<<19)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32MSB24: 
				{
					typedef HostSample<int32_t,float,-(1<<23),(1<<23)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
/*			case ASIOSTFloat32MSB: 
				{
					typedef HostSample<float,float,-1,1,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTFloat64MSB: 
				{
					typedef HostSample<double,float,-1,1,true> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}*/
			case ASIOSTInt16LSB: 
				{
					typedef HostSample<int16_t,float,-(1<<16),(1<<16)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32LSB: 
				{
					typedef HostSample<int32_t,float,-(1<<23),(1<<23)-1,8,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32LSB16: 
				{
					typedef HostSample<int32_t,float,-(1<<15),(1<<15)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32LSB18: 
				{
					typedef HostSample<int32_t,float,-(1<<17),(1<<17)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32LSB20: 
				{
					typedef HostSample<int32_t,float,-(1<<19),(1<<19)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32LSB24: 
				{
					typedef HostSample<int32_t,float,-(1<<23),(1<<23)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
/*			case ASIOSTFloat32LSB: 
				{
					typedef HostSample<float,float,-1,1,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTFloat64LSB: 
				{
					typedef HostSample<double,float,-1,1,false> AsioSmp;
					ChannelConverter<AsioSmp>::DeInterleave(interleaved,(AsioSmp**)blocks,frames,channels,stride);
					break;
				}*/
			}
		}

		static void FormatInput(ASIOSampleType type, float* interleaved, const void** blocks, unsigned frames, unsigned channels, unsigned stride)
		{
			switch(type)
			{
			case ASIOSTInt16MSB: 
				{
					typedef HostSample<int16_t,float,-(1<<15),(1<<15)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32MSB: 
				{
					typedef HostSample<int32_t,float,-(1<<24),(1<<23)-1,8,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32MSB16: 
				{
					typedef HostSample<int32_t,float,-(1<<15),(1<<15)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32MSB18: 
				{
					typedef HostSample<int32_t,float,-(1<<17),(1<<17)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32MSB20: 
				{
					typedef HostSample<int32_t,float,-(1<<19),(1<<19)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32MSB24: 
				{
					typedef HostSample<int32_t,float,-(1<<23),(1<<23)-1,0,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
/*			case ASIOSTFloat32MSB: 
				{
					typedef HostSample<float,float,-1,1,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTFloat64MSB: 
				{
					typedef HostSample<double,float,-1,1,true> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}*/
			case ASIOSTInt16LSB: 
				{
					typedef HostSample<int16_t,float,-(1<<16),(1<<16)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32LSB: 
				{
					typedef HostSample<int32_t,float,-(1<<23),(1<<23)-1,8,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32LSB16: 
				{
					typedef HostSample<int32_t,float,-(1<<15),(1<<15)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32LSB18: 
				{
					typedef HostSample<int32_t,float,-(1<<17),(1<<17)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32LSB20: 
				{
					typedef HostSample<int32_t,float,-(1<<19),(1<<19)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTInt32LSB24: 
				{
					typedef HostSample<int32_t,float,-(1<<23),(1<<23)-1,0,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
/*			case ASIOSTFloat32LSB: 
				{
					typedef HostSample<float,float,-1,1,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}
			case ASIOSTFloat64LSB: 
				{
					typedef HostSample<double,float,-1,1,false> AsioSmp;
					ChannelConverter<AsioSmp>::Interleave(interleaved,(const AsioSmp**)blocks,frames,channels,stride);
					break;
				}*/
			}
		}

		static cdecl ASIOTime* BufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess)
		{
			/* convert ASIO format to canonical format */
			void *bufferPtr[64];
			unsigned block(0);
			ASIOSampleType blockType(-1);
			unsigned strideInCanonicalBuffer = streamNumInputs;
						

			/* convert ASIO format to canonical format */
			if (streamNumInputs)
			{
				unsigned beg(0);
				blockType = channelInfos[beg].type;

				unsigned idx(1);
				while(idx<streamNumInputs)
				{
					assert(bufferInfos[idx].isInput == true && channelInfos[idx].isInput == true);
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
			}
			
			return params;
		}

		virtual const AudioStreamConfiguration& Open(const AudioStreamConfiguration& conf)
		{
			if (GetDrivers().getCurrentDriverIndex() != index)
			{
				AsioUnwind(Idle);
			}
			else
			{
				AsioUnwind(Initialized);
			}

			ASIOSetSampleRate(conf.GetSampleRate());

			/* canonicalize passed format */
			currentDelegate = &conf.GetAudioDelegate();
			currentConfiguration = conf;
			ASIOSampleRate sr;
			THROW_ERROR(DeviceOpenStreamFailure,ASIOGetSampleRate(&sr));
			currentConfiguration.SetSampleRate(sr);
			currentConfiguration.SetDeviceChannelLimits(GetNumInputs(),GetNumOutputs());

			Load();
			Init();
			Prepare();

			currentConfiguration.SetPreferredBufferSize(callbackBufferFrames);

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
		}

		virtual void Close() 
		{
			AsioUnwind(Loaded);
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
			unsigned numDrivers = GetDrivers().asioGetNumDev();

            for(unsigned i(0);i<numDrivers;++i)
			{
				try
				{
					char buffer[MaxNameLength];
					GetDrivers().asioGetDriverName(i,buffer,MaxNameLength);
					long numInputs = 0;
					long numOutputs = 0;

					GetDrivers().loadDriver(buffer);
					ASIODriverInfo driverInfo;
					ASIOSampleRate currentSampleRate;
					THROW_ERROR(DeviceInitializationFailure,ASIOInit(&driverInfo));
					THROW_ERROR(DeviceInitializationFailure,ASIOGetChannels(&numInputs,&numOutputs));
					THROW_ERROR(DeviceInitializationFailure,ASIOGetSampleRate(&currentSampleRate));

//					GetDrivers().removeCurrentDriver();
					RegisterDevice(PADInstance,AsioDevice(i,currentSampleRate,buffer,numInputs,numOutputs));
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
	} publisher;

	void Cleanup()
	{
		ASIOExit();
	}

	AudioCallbackDelegate* AsioDevice::currentDelegate = NULL;
	AudioStreamConfiguration AsioDevice::currentConfiguration;
	vector<ASIOBufferInfo> AsioDevice::bufferInfos;
	vector<ASIOChannelInfo> AsioDevice::channelInfos;
	vector<float> AsioDevice::delegateBufferInput, AsioDevice::delegateBufferOutput;
	unsigned AsioDevice::callbackBufferFrames, AsioDevice::streamNumInputs, AsioDevice::streamNumOutputs;
}
