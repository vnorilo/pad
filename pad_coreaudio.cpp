#include "PAD.h"
#include "HostAPI.h"

#include "pad_samples.h"

#include <CoreServices/CoreServices.h>
#include <CoreAudio/CoreAudio.h>
#include <AudioUnit/AudioUnit.h>

#include <string>
#include <vector>
#include <list>

#include <functional>
#include <algorithm>

#include <iostream>

#define THROW_ERROR(code,statement) {auto err = statement; if (err != noErr) throw SoftError(code,string(#statement ": ") + StatusString(err));}

namespace {
	using namespace PAD;
	using namespace std;

	string StatusString(OSStatus code) {
		char *ptr = (char *)&code;
		string tmp(ptr, ptr + 4);
		swap(tmp[0], tmp[3]);
		swap(tmp[1], tmp[2]);
		return tmp;
	}

	struct ChannelPackage {
		unsigned bufIndex, startChannel, numChannels;

	};

	class CoreAudioDevice : public AudioDevice {
		AudioDeviceID caID;
		unsigned numIns, numOuts;


		vector<ChannelPackage> inputChannelFormat;
		vector<ChannelPackage> outputChannelFormat;


		unsigned CountChannels(bool inputs, vector<ChannelPackage>& channels) {
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

		string GetName(bool input) {
			UInt32 propsize;

			AudioObjectPropertyAddress theAddress =
			{kAudioDevicePropertyDeviceName,
			input ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
			0};

			THROW_ERROR(DeviceInitializationFailure, AudioObjectGetPropertyDataSize(caID, &theAddress, 0, NULL, &propsize));
			vector<char> space(propsize);
			THROW_ERROR(DeviceInitializationFailure,
				AudioObjectGetPropertyData(caID, &theAddress, 0, NULL, &propsize, space.data( )));
			return string(space.data( ), space.data( ) + space.size( ));
		}

		unsigned numInputs, numOutputs;
		string devName;
		AudioStreamConfiguration Conform(AudioStreamConfiguration c) const {
			return c;
		}

	public:
		CoreAudioDevice(AudioDeviceID id)
			:caID(id), numInputs(CountChannels(true, inputChannelFormat)), numOutputs(CountChannels(false, outputChannelFormat)),
			devName(GetName(true) + "/" + GetName(false)) { }

        ~CoreAudioDevice() { Close(); }
		const char* GetName( ) const { return devName.c_str( ); }
		unsigned GetNumInputs( ) const { return numInputs; }
		unsigned GetNumOutputs( ) const { return numOutputs; }
		const char *GetHostAPI( ) const { return "CoreAudio"; }

		AudioStreamConfiguration DefaultMono( ) const { return Conform(AudioStreamConfiguration(44100).Input(0).Output(0)); }
		AudioStreamConfiguration DefaultStereo( ) const { return Conform(AudioStreamConfiguration(44100).StereoInput(0).StereoOutput(0)); }
		AudioStreamConfiguration DefaultAllChannels( ) const { return Conform(AudioStreamConfiguration(44100).Inputs(ChannelRange(0, numInputs)).Outputs(ChannelRange(0, numOutputs))); }

		AudioStreamConfiguration currentConfiguration;

		AudioUnit AUHAL=nullptr;

		static void CopyChannelBundle(void *dest, const void *src, unsigned copySz, unsigned destStride, unsigned srcStride, unsigned frames) {
			char *destb = (char *)dest;
			char *srcb = (char *)src;
			for (unsigned i(0); i < frames; ++i) {
				memcpy(destb + destStride * i, srcb + srcStride * i, copySz);
			}
		}


		vector<float> delegateInputBuffer;
		std::uint64_t padTimeStamp = 0;

		OSStatus AUHALProc(AudioUnitRenderActionFlags* ioFlags, const AudioTimeStamp *timeStamp, UInt32 Bus, UInt32 frames, AudioBufferList *io) {
			if (Bus == 0) {
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

				if (GetBufferSwitchLock( )) {
					std::lock_guard<recursive_mutex> lock(*GetBufferSwitchLock( ));
                    
                    BufferSwitch(IO{currentConfiguration, delegateInputBuffer.data(), (float*)io->mBuffers[0].mData, padTimeStamp, frames});
                                 
                } else BufferSwitch(IO{currentConfiguration, delegateInputBuffer.data(), (float*)io->mBuffers[0].mData, padTimeStamp, frames});

		padTimeStamp += frames;
			}
			return noErr;
		}


		static OSStatus AUHALCallback(void *inRefCon, AudioUnitRenderActionFlags* ioFlags, const AudioTimeStamp *timeStamp, UInt32 Bus, UInt32 frames, AudioBufferList *io) {
			CoreAudioDevice *cadev = (CoreAudioDevice*)inRefCon;
			return cadev->AUHALProc(ioFlags, timeStamp, Bus, frames, io);
		}

		const AudioStreamConfiguration& Open(const AudioStreamConfiguration& c) {

			currentConfiguration = c;

			AudioComponentDescription desc = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
			AudioComponent comp = AudioComponentFindNext(NULL, &desc);
			if (comp == NULL) throw SoftError(DeviceInitializationFailure, "Can't open CoreAudio I/O AudioUnit");

			THROW_ERROR(DeviceInitializationFailure, AudioComponentInstanceNew(comp, &AUHAL));
			//THROW_ERROR(DeviceInitializationFailure, AudioUnitInitialize(AUHAL));

			UInt32 enable = 1;
			THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &caID, sizeof(caID)));
			THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 1, &caID, sizeof(caID)));

			THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enable, sizeof(enable)));
			THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &enable, sizeof(enable)));

			AudioStreamBasicDescription inputFmt = {
				currentConfiguration.GetSampleRate( ), 'lpcm',
				kLinearPCMFormatFlagIsFloat + kLinearPCMFormatFlagIsPacked,
				UInt32(sizeof(float)*numInputs), 1,
				UInt32(sizeof(float)*numInputs), numInputs, 32, 0};

			AudioStreamBasicDescription outputFmt = inputFmt;
			outputFmt.mChannelsPerFrame = numOutputs;
			outputFmt.mBytesPerFrame = sizeof(float)*numOutputs;
			outputFmt.mBytesPerPacket = outputFmt.mBytesPerFrame;

			// AUHAL unit inputs are hardware outputs and vice versa
			THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &outputFmt, sizeof(AudioStreamBasicDescription)));
			THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &inputFmt, sizeof(AudioStreamBasicDescription)));

			vector<SInt32> channelMap;
			unsigned streamChannel(0);
			unsigned numDevIns(currentConfiguration.GetNumDeviceInputs( )), numDevOuts(currentConfiguration.GetNumDeviceOutputs( ));
			for (unsigned i(0); i < numDevIns; ++i) {
				if (currentConfiguration.IsInputEnabled(i)) channelMap.push_back(streamChannel++);
				else channelMap.push_back(-1);
			}

			THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output, 1, channelMap.data( ), UInt32(channelMap.size( )*sizeof(SInt32))));

			streamChannel = 0;
			channelMap.clear( );
			for (unsigned i(0); i < numDevOuts; ++i) {
				if (currentConfiguration.IsOutputEnabled(i)) channelMap.push_back(streamChannel++);
				else channelMap.push_back(-1);
			}

			THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Input, 0, channelMap.data( ), UInt32(channelMap.size( )*sizeof(SInt32))));

			AURenderCallbackStruct cb = {AUHALCallback, this};
			THROW_ERROR(DeviceInitializationFailure, AudioUnitSetProperty(AUHAL, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(AURenderCallbackStruct)));

			THROW_ERROR(DeviceInitializationFailure, AudioUnitInitialize(AUHAL));

			padTimeStamp = 0ul;
			if (currentConfiguration.HasSuspendOnStartup( ) == false) Resume( );

			return currentConfiguration;
		}

		void Resume( ) {
			THROW_ERROR(DeviceStartStreamFailure, AudioOutputUnitStart(AUHAL));
		}

		void Suspend( ) {
			THROW_ERROR(DeviceStopStreamFailure, AudioOutputUnitStop(AUHAL));
		}

		void Close( )
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

		bool Supports(const AudioStreamConfiguration&) const { return false; }

		double CPU_Load( ) const { return 0.0; }
	};


	class CoreaudioPublisher : public HostAPIPublisher {
		list<CoreAudioDevice> devices;
	public:
		void RegisterDevice(Session& PI, const CoreAudioDevice& dev) {
			devices.push_back(dev);
			PI.Register(&devices.back( ));
		}

		void Publish(Session& PADInstance, DeviceErrorDelegate& errorHandler) {
			UInt32 propsize(0);
			AudioObjectPropertyAddress theAddress = {kAudioHardwarePropertyDevices,
				kAudioObjectPropertyScopeGlobal,
				kAudioObjectPropertyElementMaster};

			THROW_ERROR(DeviceInitializationFailure, AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &theAddress, 0, NULL, &propsize));

			vector<AudioDeviceID> devids(propsize / sizeof(AudioDeviceID));
			THROW_ERROR(DeviceInitializationFailure, AudioObjectGetPropertyData(kAudioObjectSystemObject, &theAddress, 0, NULL, &propsize, devids.data( )));

			for (auto dev : devids) {
				try {
					RegisterDevice(PADInstance, CoreAudioDevice(dev));
				} catch (SoftError se) {
					errorHandler.Catch(se);
				} catch (HardError he) {
					errorHandler.Catch(he);
				}
			}
		}
		const char *GetName( ) const { return "CoreAudio"; }
	} publisher;
}

namespace PAD {
	IHostAPI* LinkCoreAudio( ) {
		return &publisher;
	}
}
