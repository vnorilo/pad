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

#define THROW_ERROR(code,expr) {ASIOError err = (expr); if (err != ASE_OK) throw PAD::SoftError(code,std::string(#expr " failed with ASIO error ") + utostr(err) );}
#define THROW_FALSE(code,expr) {if (expr == false) throw PAD::SoftError(code,#expr " failed");}
#define THROW_TRUE(code,expr) {if (expr == true) throw PAD::SoftError(code,#expr " failed");}

namespace {
	using namespace std;
	using namespace PAD;
	
	AsioDrivers drivers;

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
			if (to == Running) return;
			THROW_ERROR(DeviceStopStreamFailure,ASIOStop());
		case Prepared:
			if (to == Prepared) return;
			THROW_ERROR(DeviceCloseStreamFailure,ASIODisposeBuffers());
		case Initialized:
			if (to == Initialized) return;
			THROW_ERROR(DeviceDeinitializationFailure,ASIOExit());
		case Loaded:
			if (to == Loaded) return;
			drivers.removeCurrentDriver();
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
			AsioUnwind(Idle);
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

		static ASIODriverInfo driverInfo;
		static AudioStreamConfiguration currentConfiguration;
		static AudioCallbackDelegate* currentDelegate;
		static vector<ASIOBufferInfo> bufferInfos;
		static vector<ASIOChannelInfo> channelInfos;
		static vector<float> delegateBufferInput, delegateBufferOutput;
		static unsigned callbackBufferFrames, streamNumInputs, streamNumOutputs;

		static void BufferSwitch(long doubleBufferIndex, ASIOBool directProcess)
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
				THROW_ERROR(DeviceDriverFailure,drivers.asioGetDriverName(index,space.data(),space.size()));
				THROW_TRUE(DeviceDriverFailure,drivers.loadDriver(space.data()));
				State = Loaded;
			}
		}

		void Init()
		{
			if (State < Initialized)
			{
				THROW_ERROR(DeviceInitializationFailure,ASIOInit(&driverInfo));
			}
		}

		void Prepare()
		{
			if (State < Prepared)
			{
				long minBuf, maxBuf, prefBuf, bufGran;
				ASIOCallbacks asioCb = {BufferSwitch,SampleRateDidChange,AsioMessage,BufferSwitchTimeInfo};
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

			}
		}

		void Run()
		{
			if (State < Running)
			{
				ASIOStart();
			}
		}

		static ASIOTime* BufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess)
		{
			/* convert ASIO format to canonical format */
			void *blockBuffer[4];
			unsigned block(0);
			ASIOSampleType blockType(-1);
			unsigned strideInCanonicalBuffer = streamNumInputs;
						

			currentDelegate->Process(0ll,currentConfiguration,
									 delegateBufferInput.data(),
									 delegateBufferOutput.data(),
									 callbackBufferFrames);

			/* convert canonical format to ASIO format */
			if (streamNumOutputs)
			{
				blockType = channelInfos[bufferInfos[0].channelNum].type;
				for(unsigned i(0);i<streamNumOutputs;++i)
				{
					if (channelInfos[bufferInfos[i].channelNum].type == blockType)
					{
						blockBuffer[block++] = bufferInfos[i].buffers[doubleBufferIndex];
						if (block == 4)
						{

						}
					}

				}
			}
			
			return params;
		}

		virtual const AudioStreamConfiguration& Open(const AudioStreamConfiguration& conf, AudioCallbackDelegate& cb, bool startSuspended = false)
		{
			if (drivers.getCurrentDriverIndex() != index)
			{
				AsioUnwind(Idle);
			}
			else
			{
				AsioUnwind(Initialized);
			}

			currentDelegate = &cb;
			currentConfiguration = conf;

			Load();
			Init();
			Prepare();

			if (startSuspended == false) Run();
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
			unsigned numDrivers = drivers.asioGetNumDev();

            for(unsigned i(0);i<numDrivers;++i)
			{
				try
				{
					char buffer[MaxNameLength];
					drivers.asioGetDriverName(i,buffer,MaxNameLength);
					long numInputs = 0;
					long numOutputs = 0;

					drivers.loadDriver(buffer);
					ASIODriverInfo driverInfo;
					ASIOSampleRate currentSampleRate;
					THROW_ERROR(DeviceInitializationFailure,ASIOInit(&driverInfo));
					THROW_ERROR(DeviceInitializationFailure,ASIOGetChannels(&numInputs,&numOutputs));
					THROW_ERROR(DeviceInitializationFailure,ASIOGetSampleRate(&currentSampleRate));

					drivers.removeCurrentDriver();
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

	AudioCallbackDelegate* AsioDevice::currentDelegate = NULL;
	AudioStreamConfiguration AsioDevice::currentConfiguration;
	ASIODriverInfo AsioDevice::driverInfo;
	vector<ASIOBufferInfo> AsioDevice::bufferInfos;
	vector<ASIOChannelInfo> AsioDevice::channelInfos;
	vector<float> AsioDevice::delegateBufferInput, AsioDevice::delegateBufferOutput;
	unsigned AsioDevice::callbackBufferFrames, AsioDevice::streamNumInputs, AsioDevice::streamNumOutputs;
}
