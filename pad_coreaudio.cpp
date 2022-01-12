#include "pad.h"
#include "HostAPI.h"

#include "pad_samples.h"

#include <CoreServices/CoreServices.h>
#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>
#include <mach/mach_time.h>

#include <string>
#include <vector>
#include <list>

#include <functional>
#include <algorithm>

#include <iostream>

#include <chrono>

#define STRINGIZE(x) STR_EXPAND(x)
#define STR_EXPAND(x) #x

#define THROW_ERROR(code,statement) { \
    auto err = statement; \
    if (err != noErr) { \
        throw SoftError(code, StatusString(err) + " " #statement " (" STRINGIZE(__LINE__) ")" ); \
    } \
}

namespace {
	using namespace PAD;
	using namespace std;

	string StatusString(OSStatus code) {
        CFStringRef str = SecCopyErrorMessageString(code, nullptr);
        std::string s {CFStringGetCStringPtr(str, kCFStringEncodingUTF8)};
        CFRelease(str);
        return s;
	}

	struct ChannelPackage {
		unsigned bufIndex, startChannel, numChannels;

	};

	class CoreAudioDevice : public AudioDevice {
		AudioDeviceID inputId, outputId;

		vector<ChannelPackage> inputChannelFormat;
		vector<ChannelPackage> outputChannelFormat;


		unsigned CountChannels(AudioDeviceID caID, bool inputs, vector<ChannelPackage>& channels) {
			UInt32 propsize;
			unsigned result = 0;

			AudioObjectPropertyAddress theAddress =
			{kAudioDevicePropertyStreamConfiguration,
			inputs ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
			0};

			THROW_ERROR(DeviceInitializationFailure, AudioObjectGetPropertyDataSize(caID, &theAddress, 0, NULL, &propsize));
			vector<uint8_t> space(propsize);
			AudioBufferList *buflist = (AudioBufferList*)space.data( );
			THROW_ERROR(DeviceInitializationFailure, AudioObjectGetPropertyData(caID, &theAddress, 0, NULL, &propsize, buflist));
			for (unsigned i(0); i < buflist->mNumberBuffers; ++i) {
				result += buflist->mBuffers[i].mNumberChannels;
			}
			return result;
		}

		string GetName(AudioDeviceID caID, bool input) {
			UInt32 propsize;

			AudioObjectPropertyAddress theAddress =
			{kAudioDevicePropertyDeviceName,
			input ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
			0};

			THROW_ERROR(DeviceInitializationFailure, AudioObjectGetPropertyDataSize(caID, &theAddress, 0, NULL, &propsize));
			vector<char> space(propsize);
			THROW_ERROR(DeviceInitializationFailure,
				AudioObjectGetPropertyData(caID, &theAddress, 0, NULL, &propsize, space.data( )));
			return string(space.data( ), space.data( ) + space.size( ) - 1);
		}

		unsigned numInputs, numOutputs;
		string devName;
		AudioStreamConfiguration Conform(AudioStreamConfiguration c) const {
			return c;
		}
        
        static mach_timebase_info_data_t timebaseInfo;

	public:
		CoreAudioDevice(AudioDeviceID input, AudioDeviceID output)
			:inputId(input), outputId(output),
             numInputs(CountChannels(input, true, inputChannelFormat)),
             numOutputs(CountChannels(output, false, outputChannelFormat)) {
                
            auto inputName = GetName(input, true);
            auto outputName = GetName(output, false);
            
            if (inputName == outputName) devName = inputName;
            else devName = inputName + "/" + outputName;
                
            mach_timebase_info(&timebaseInfo);
        }

        ~CoreAudioDevice() { Close(); }
        
        AudioDeviceID CoreAudioInputID() const {
            return inputId;
        }
        
        AudioDeviceID CoreAudioOutputID() const {
            return outputId;
        }
        
		const char* GetName( ) const override { return devName.c_str( ); }
		unsigned GetNumInputs( ) const override { return numInputs; }
		unsigned GetNumOutputs( ) const override { return numOutputs; }
		const char *GetHostAPI( ) const override { return "CoreAudio"; }

		AudioStreamConfiguration DefaultMono( ) const override { return Conform(AudioStreamConfiguration(44100).Input(0).Output(0)); }
		AudioStreamConfiguration DefaultStereo( ) const override  { return Conform(AudioStreamConfiguration(44100).StereoInput(0).StereoOutput(0)); }
		AudioStreamConfiguration DefaultAllChannels( ) const override  { return Conform(AudioStreamConfiguration(44100).Inputs(ChannelRange(0, numInputs)).Outputs(ChannelRange(0, numOutputs))); }

		AudioStreamConfiguration currentConfiguration;

		AudioUnit AUHAL=nullptr;
        UInt32 callbackBus;

		static void CopyChannelBundle(void *dest, const void *src, unsigned copySz, unsigned destStride, unsigned srcStride, unsigned frames) {
			char *destb = (char *)dest;
			char *srcb = (char *)src;
			for (unsigned i(0); i < frames; ++i) {
				memcpy(destb + destStride * i, srcb + srcStride * i, copySz);
			}
		}


		vector<float> delegateInputBuffer;

		OSStatus AUHALProc(AudioUnitRenderActionFlags* ioFlags, const AudioTimeStamp *timeStamp, UInt32 Bus, UInt32 frames, AudioBufferList *io) {
            
            if (callbackBus != Bus) return noErr;
            
            if (delegateInputBuffer.size( ) < frames * currentConfiguration.GetNumStreamInputs( )) {
                delegateInputBuffer.resize(frames*currentConfiguration.GetNumStreamInputs( ));
            }

            if (currentConfiguration.GetNumStreamInputs( ) > 0) {
                AudioBufferList ab;
                ab.mNumberBuffers = 1;
                ab.mBuffers[0].mNumberChannels = currentConfiguration.GetNumStreamInputs( );
                ab.mBuffers[0].mDataByteSize = sizeof(float) * currentConfiguration.GetNumStreamInputs( ) * frames;
                ab.mBuffers[0].mData = delegateInputBuffer.data( );

                OSStatus err = AudioUnitRender(AUHAL, ioFlags, timeStamp, 1, frames, &ab);
                if (err != noErr) {

                }
            }
            
            auto sysTime = timeStamp->mHostTime;
            sysTime *= timebaseInfo.numer;
            sysTime /= timebaseInfo.denom; // nanosec to usec
            
            auto outputTime = std::chrono::microseconds((sysTime + 500) / 1000);
            auto inputTime = outputTime; // todo: compute latency!!

            float *outputBuffer = nullptr;
            if (io && io->mNumberBuffers) {
                outputBuffer = (float*)io->mBuffers[0].mData;
            }
            
            IO ioData{currentConfiguration, delegateInputBuffer.data(), outputBuffer, frames, inputTime, outputTime};
            if (GetBufferSwitchLock( )) {
                std::lock_guard<recursive_mutex> lock(*GetBufferSwitchLock( ));
                BufferSwitch(ioData);
            } else BufferSwitch(ioData);

            return noErr;
		}
        
        static std::chrono::microseconds GetTime() {
            auto sysTime = mach_absolute_time();
            sysTime *= timebaseInfo.numer;
            sysTime /= timebaseInfo.denom; // nanosec to usec
            return std::chrono::microseconds(sysTime / 1000);
        }

		std::chrono::microseconds DeviceTimeNow() const override {
			return GetTime();
		}

		GetDeviceTime GetDeviceTimeCallback() const override {
			return GetTime;
		}


		static OSStatus AUHALCallback(void *inRefCon, AudioUnitRenderActionFlags* ioFlags, const AudioTimeStamp *timeStamp, UInt32 Bus, UInt32 frames, AudioBufferList *io) {
			CoreAudioDevice *cadev = (CoreAudioDevice*)inRefCon;
			return cadev->AUHALProc(ioFlags, timeStamp, Bus, frames, io);
		}

		const AudioStreamConfiguration& Open(const AudioStreamConfiguration& c) override {

			currentConfiguration = c;

			AudioComponentDescription desc = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
			AudioComponent comp = AudioComponentFindNext(NULL, &desc);
			if (comp == NULL) throw SoftError(DeviceInitializationFailure, "Can't open CoreAudio I/O AudioUnit");

			THROW_ERROR(DeviceInitializationFailure, AudioComponentInstanceNew(comp, &AUHAL));
			//THROW_ERROR(DeviceInitializationFailure, AudioUnitInitialize(AUHAL));


            UInt32 numStreamIns = c.GetNumStreamInputs();
            UInt32 numStreamOuts = c.GetNumStreamOutputs();
            UInt32 enable = 1, disable = 0;
            
            AudioStreamBasicDescription inputFmt = {
                currentConfiguration.GetSampleRate( ), 'lpcm',
                kLinearPCMFormatFlagIsFloat + kLinearPCMFormatFlagIsPacked,
                UInt32(sizeof(float)*numStreamIns), 1,
                UInt32(sizeof(float)*numStreamIns), numStreamIns, 32, 0};
            
            AudioStreamBasicDescription outputFmt = {
                currentConfiguration.GetSampleRate( ), 'lpcm',
                kLinearPCMFormatFlagIsFloat + kLinearPCMFormatFlagIsPacked,
                UInt32(sizeof(float)*numStreamOuts), 1,
                UInt32(sizeof(float)*numStreamOuts), numStreamOuts, 32, 0};

            
			// AUHAL unit inputs are hardware outputs and vice versa

            AURenderCallbackStruct cb = {AUHALCallback, this};
            
            if (numStreamOuts) {
                THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_EnableIO,
                                                                              kAudioUnitScope_Output, 0, &enable, sizeof(enable)));
            } else {
                THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_EnableIO,
                                                                              kAudioUnitScope_Output, 0, &disable, sizeof(enable)));
            }
            
            if (numStreamIns) {
                THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_EnableIO,
                                                                              kAudioUnitScope_Input, 1, &enable, sizeof(enable)));
            } else {
                THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_EnableIO,
                                                                              kAudioUnitScope_Input, 1, &disable, sizeof(enable)));
            }

            if (numStreamOuts) {
                THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &outputId, sizeof(outputId)));
                THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &outputFmt, sizeof(AudioStreamBasicDescription)));

                vector<SInt32> channelMap;
                unsigned numDevOuts(currentConfiguration.GetNumDeviceOutputs( ));
                unsigned streamChannel(0);
                for (unsigned i(0); i < numDevOuts; ++i) {
                    if (currentConfiguration.IsOutputEnabled(i)) channelMap.push_back(streamChannel++);
                    else channelMap.push_back(-1);
                }
                
                callbackBus = 0;
                THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Input, 0, channelMap.data( ), UInt32(channelMap.size( )*sizeof(SInt32))));
                THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Global, 0, &cb, sizeof(AURenderCallbackStruct)));
            }
            
            
            if (numStreamIns) {
                THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 1, &inputId, sizeof(inputId)));
                THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &inputFmt, sizeof(AudioStreamBasicDescription)));

                vector<SInt32> channelMap;
                unsigned streamChannel(0);
                unsigned numDevIns(currentConfiguration.GetNumDeviceInputs( ));
                for (unsigned i(0); i < numDevIns; ++i) {
                    if (currentConfiguration.IsInputEnabled(i)) channelMap.push_back(streamChannel++);
                    else channelMap.push_back(-1);
                }
                
                THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output, 1, channelMap.data( ), UInt32(channelMap.size( )*sizeof(SInt32))));
                
                if (!numStreamOuts) {
                    callbackBus = 1;
                    THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 1, &cb, sizeof(AURenderCallbackStruct)));
                }
            }

			THROW_ERROR(DeviceInitializationFailure, AudioUnitInitialize(AUHAL));

			if (currentConfiguration.HasSuspendOnStartup( ) == false) Resume( );

			return currentConfiguration;
		}

		void Resume( ) override {
			THROW_ERROR(DeviceStartStreamFailure, AudioOutputUnitStart(AUHAL));
		}

		void Suspend( ) override {
			THROW_ERROR(DeviceStopStreamFailure, AudioOutputUnitStop(AUHAL));
		}

		void Close( ) override
        {
            if (AUHAL!=nullptr)
            {
                if (AudioUnitUninitialize (AUHAL) == noErr)
                {
                    AUHAL=nullptr;
                    StreamDidEnd();
                } else throw SoftError(DeviceCloseStreamFailure,"Could not stop stream");
            }
        }

		bool Supports(const AudioStreamConfiguration&) const override { return false; }

		double CPU_Load( ) const override { return 0.0; }
	};


	class CoreaudioPublisher : public HostAPIPublisher {
		list<CoreAudioDevice> devices;
	public:
		void Publish(Session& PADInstance, DeviceErrorDelegate& errorHandler) {
			UInt32 propsize(0);
			AudioObjectPropertyAddress theAddress = {kAudioHardwarePropertyDevices,
				kAudioObjectPropertyScopeGlobal,
				kAudioObjectPropertyElementMaster};

			THROW_ERROR(DeviceInitializationFailure, AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &theAddress, 0, NULL, &propsize));

			vector<AudioDeviceID> devids(propsize / sizeof(AudioDeviceID));
			THROW_ERROR(DeviceInitializationFailure, AudioObjectGetPropertyData(kAudioObjectSystemObject, &theAddress, 0, NULL, &propsize, devids.data( )));
            
            AudioDeviceID defaultOutputDevice = 0, defaultInputDevice = 0;
            theAddress = { kAudioHardwarePropertyDefaultOutputDevice,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMaster };
            
            AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &theAddress, 0, NULL, &propsize);
            AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                       &theAddress,
                                       0,
                                       NULL,
                                       &propsize,
                                       &defaultOutputDevice);

            theAddress = { kAudioHardwarePropertyDefaultInputDevice,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMaster };

            AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &theAddress, 0, NULL, &propsize);
            AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                       &theAddress,
                                       0,
                                       NULL,
                                       &propsize,
                                       &defaultInputDevice);
            
            for (auto dev : devids) {
				try {
					devices.emplace_back(dev, dev);
				} catch (SoftError se) {
					errorHandler.Catch(se);
				} catch (HardError he) {
					errorHandler.Catch(he);
				}
			}
            
			devices.sort([=](const CoreAudioDevice& a, const CoreAudioDevice& b) {
                
                int aDefault = a.CoreAudioInputID() == defaultInputDevice ? 1 : 0;
                aDefault += a.CoreAudioOutputID() == defaultOutputDevice ? 1 : 0;
                
                int bDefault = b.CoreAudioInputID() == defaultInputDevice ? 1 : 0;
                bDefault += b.CoreAudioOutputID() == defaultOutputDevice ? 1 : 0;
                
                if (aDefault > bDefault) return true;
                if (aDefault < bDefault) return false;
                
				if (a.GetNumOutputs() > b.GetNumOutputs()) return true;
				if (a.GetNumOutputs() < b.GetNumOutputs()) return false;
				return a.GetNumInputs() > b.GetNumInputs();
			});

			for(auto& d : devices) {
				PADInstance.Register(&d);
			}
		}
		const char *GetName( ) const { return "CoreAudio"; }
	} publisher;

    mach_timebase_info_data_t CoreAudioDevice::timebaseInfo;
}

namespace PAD {
	IHostAPI* LinkCoreAudio( ) {
		return &publisher;
	}
}
