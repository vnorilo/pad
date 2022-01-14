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
            
            space.back() = 0;
			return string(space.data( ));
		}

		unsigned numInputs, numOutputs;
		string devName;
		AudioStreamConfiguration Conform(AudioStreamConfiguration c) const {
            if (c.GetNumStreamInputs() > numInputs) {
                c.SetInputRanges({ChannelRange{0, numInputs}});
            }
            
            if (c.GetNumStreamOutputs() > numOutputs) {
                    c.SetOutputRanges({ChannelRange{0, numOutputs}});
            }
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
        
        bool AggregateContains(AudioDeviceID aggregate, AudioDeviceID device) const {
            
            if (aggregate == device) return true;

            AudioObjectPropertyAddress addr = {
                kAudioAggregateDevicePropertyActiveSubDeviceList,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMaster
            };

            UInt32 size = 0;
            if (AudioObjectGetPropertyDataSize(aggregate, &addr, 0, NULL, &size) == noErr) {
                std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
                if (AudioObjectGetPropertyData(aggregate, &addr, 0, NULL, &size, devices.data()) == noErr) {
                    return std::find(devices.begin(), devices.end(), device) != devices.end();
                }
            }
            
            return false;

        }
                
        bool ContainsInputId(AudioDeviceID input) const {
            return AggregateContains(inputId, input);
        }
        
        bool ContainsOutputId(AudioDeviceID output) const {
            return AggregateContains(outputId, output);
        }
        
		const char* GetName( ) const override { return devName.c_str( ); }
		unsigned GetNumInputs( ) const override { return numInputs; }
		unsigned GetNumOutputs( ) const override { return numOutputs; }
		const char *GetHostAPI( ) const override { return "CoreAudio"; }

        float defaultSampleRate = 44100;

        float GetDefaultSampleRate() {
            return defaultSampleRate;
        }
        
        void SetDefaultSampleRate(float sr) {
            defaultSampleRate = sr;
        }

		AudioStreamConfiguration DefaultMono( ) const override { return Conform(AudioStreamConfiguration(defaultSampleRate).Input(0).Output(0)); }
		AudioStreamConfiguration DefaultStereo( ) const override  { return Conform(AudioStreamConfiguration(defaultSampleRate).StereoInput(0).StereoOutput(0)); }
		AudioStreamConfiguration DefaultAllChannels( ) const override  { return Conform(AudioStreamConfiguration(defaultSampleRate).Inputs(ChannelRange(0, numInputs)).Outputs(ChannelRange(0, numOutputs))); }

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

            auto sysTime = timeStamp->mHostTime;
            sysTime *= timebaseInfo.numer;
            sysTime /= timebaseInfo.denom; // nanosec to usec
            
            auto outputTime = std::chrono::microseconds((sysTime + 500) / 1000);
            auto inputTime = outputTime; // todo: compute latency!!
            
            if (currentConfiguration.GetNumStreamInputs( ) > 0) {
                AudioBufferList ab;
                ab.mNumberBuffers = 1;
                ab.mBuffers[0].mNumberChannels = currentConfiguration.GetNumStreamInputs( );
                ab.mBuffers[0].mDataByteSize = sizeof(float) * currentConfiguration.GetNumStreamInputs( ) * frames;
                ab.mBuffers[0].mData = delegateInputBuffer.data( );

                OSStatus err = AudioUnitRender(AUHAL, ioFlags, timeStamp, 1, frames, &ab);
                if (err != noErr) {
                    for(auto &s : delegateInputBuffer) s = 0;
                    inputTime = std::chrono::microseconds(-1);
                }
            }
            
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
            
            Float64 sampleRate = c.GetSampleRate();
            UInt32 srSize = sizeof(sampleRate);
            
			THROW_ERROR(DeviceInitializationFailure,
                        AudioComponentInstanceNew(comp, &AUHAL));
			//THROW_ERROR(DeviceInitializationFailure, AudioUnitInitialize(AUHAL));

            UInt32 numStreamIns = c.GetNumStreamInputs();
            UInt32 numStreamOuts = c.GetNumStreamOutputs();
            UInt32 enable = 1, disable = 0;

			// AUHAL unit inputs are hardware outputs and vice versa
            AURenderCallbackStruct cb = {AUHALCallback, this};
            
            if (!numStreamOuts) {
                THROW_ERROR(DeviceInitializationFailure,
                            AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_EnableIO,
                                                 kAudioUnitScope_Output, 0, &disable, sizeof(disable)));
            }
            
            if (numStreamIns) {
                THROW_ERROR(DeviceInitializationFailure,
                            AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_EnableIO,
                                                 kAudioUnitScope_Input, 1, &enable, sizeof(enable)));
            }
            
            callbackBus = 1;
            AudioUnitPropertyID callbackStyle = kAudioOutputUnitProperty_SetInputCallback;
            UInt32 maximumFrames = 1024;

            if (numStreamOuts) {

                AudioStreamBasicDescription outputFmt = {
                    currentConfiguration.GetSampleRate( ), 'lpcm',
                    kLinearPCMFormatFlagIsFloat + kLinearPCMFormatFlagIsPacked,
                    UInt32(sizeof(float)*numStreamOuts), 1,
                    UInt32(sizeof(float)*numStreamOuts), numStreamOuts, 32, 0};

                THROW_ERROR(DeviceInitializationFailure,
                            AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &outputId, sizeof(outputId)));
                THROW_ERROR(DeviceInitializationFailure,
                            AudioUnitSetProperty(AUHAL, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &outputFmt, sizeof(AudioStreamBasicDescription)));
                UInt32 size = sizeof(maximumFrames);
                THROW_ERROR(DeviceInitializationFailure,
                            AudioUnitGetProperty(AUHAL, kAudioUnitProperty_MaximumFramesPerSlice,
                                                 kAudioUnitScope_Global, 0, &maximumFrames, &size));

                vector<SInt32> channelMap;
                unsigned numDevOuts(currentConfiguration.GetNumDeviceOutputs( ));
                unsigned streamChannel(0);
                for (unsigned i(0); i < numDevOuts; ++i) {
                    if (currentConfiguration.IsOutputEnabled(i)) channelMap.push_back(streamChannel++);
                    else channelMap.push_back(-1);
                }
                
                callbackBus = 0;
                callbackStyle = kAudioUnitProperty_SetRenderCallback;
                
                THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Input, 0, channelMap.data( ), UInt32(channelMap.size( )*sizeof(SInt32))));
            }

            if (numStreamIns) {
                
                AudioStreamBasicDescription inputFmt = {
                    currentConfiguration.GetSampleRate( ), 'lpcm',
                    kLinearPCMFormatFlagIsFloat + kLinearPCMFormatFlagIsPacked,
                    UInt32(sizeof(float)*numStreamIns), 1,
                    UInt32(sizeof(float)*numStreamIns), numStreamIns, 32, 0};
                
                THROW_ERROR(DeviceInitializationFailure,
                            AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 1, &inputId, sizeof(inputId)));
                THROW_ERROR(DeviceInitializationFailure,
                            AudioUnitSetProperty(AUHAL, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &inputFmt, sizeof(AudioStreamBasicDescription)));
                THROW_ERROR(DeviceInitializationFailure,
                            AudioUnitSetProperty(AUHAL, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Output, 1, &maximumFrames, sizeof(maximumFrames)));
                
                vector<SInt32> channelMap;
                unsigned streamChannel(0);
                unsigned numDevIns(currentConfiguration.GetNumDeviceInputs( ));
                for (unsigned i(0); i < numDevIns; ++i) {
                    if (currentConfiguration.IsInputEnabled(i)) channelMap.push_back(streamChannel++);
                    else channelMap.push_back(-1);
                }
                
                THROW_ERROR(DeviceInitializationFailure,
                            AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output, 1, channelMap.data( ), UInt32(channelMap.size( )*sizeof(SInt32))));
                
            }

            THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, callbackStyle,
                                                                          kAudioUnitScope_Global,
                                                                          callbackBus,
                                                                          &cb, sizeof(AURenderCallbackStruct)));
            
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

    AudioObjectPropertySelector mSelector;
    AudioObjectPropertyScope    mScope;
    AudioObjectPropertyElement  mElement;

    template <typename T>
    std::vector<T> GetPropertyVector(AudioObjectID object,
                                     AudioObjectPropertySelector sel,
                                     AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
        
        AudioObjectPropertyAddress theAddress = { sel, scope, object };
        
        UInt32 size = 0;
        THROW_ERROR(DeviceInitializationFailure, AudioObjectGetPropertyDataSize(object, &theAddress, 0, nullptr, &size));
        std::vector<T> data(size / sizeof(T));
        THROW_ERROR(DeviceInitializationFailure, AudioObjectGetPropertyData(object, &theAddress, 0, nullptr, &size, data.data()));
        return data;
    }

    template <typename T> T GetProperty(AudioObjectID object,
                                        AudioObjectPropertySelector sel,
                                        AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
        
        AudioObjectPropertyAddress theAddress = { sel, scope, object };
        
        UInt32 size = 0;
        THROW_ERROR(DeviceInitializationFailure, AudioObjectGetPropertyDataSize(object, &theAddress, 0, nullptr, &size));
        T data;
        THROW_ERROR(DeviceInitializationFailure, AudioObjectGetPropertyData(object, &theAddress, 0, nullptr, &size, &data));
        return data;
    }



	class CoreaudioPublisher : public HostAPIPublisher {
		list<CoreAudioDevice> devices;
	public:
        
		void Publish(Session& PADInstance, DeviceErrorDelegate& errorHandler) {
            
            auto devids = GetPropertyVector<AudioDeviceID>(kAudioObjectSystemObject, kAudioHardwarePropertyDevices);
            auto defaultOutputDevice = GetProperty<AudioDeviceID>(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice);
            auto defaultInputDevice = GetProperty<AudioDeviceID>(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultInputDevice);
                                    
            for (auto dev : devids) {
				try {
					devices.emplace_back(dev, dev);
                    
                    try {
                        devices.back().SetDefaultSampleRate(GetProperty<Float64>(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal));
                        devices.back().SetDefaultSampleRate(GetProperty<Float64>(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeInput));
                    } catch(...) { }
                    
				} catch (SoftError se) {
					errorHandler.Catch(se);
				} catch (HardError he) {
					errorHandler.Catch(he);
				}
			}
            
			devices.sort([=](const CoreAudioDevice& a, const CoreAudioDevice& b) {
                
                int aDefault = a.ContainsInputId(defaultInputDevice) ? 1 : 0;
                aDefault += a.ContainsOutputId(defaultOutputDevice) ? 1 : 0;
                
                int bDefault = b.ContainsInputId(defaultInputDevice) ? 1 : 0;
                bDefault += b.ContainsOutputId(defaultOutputDevice) ? 1 : 0;

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
