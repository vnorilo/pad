#include "PAD.h"
#include "HostAPI.h"

#include <CoreServices/CoreServices.h>
#include <CoreAudio/CoreAudio.h>

#include <string>
#include <vector>
#include <list>

#include <functional>
#include <algorithm>

#define THROW_ERROR(code,statement) statement;

namespace {
    using namespace PAD;
    using namespace std;
    class CoreAudioDevice : public AudioDevice{
        AudioDeviceID caID;
        unsigned numIns, numOuts;
        unsigned CountChannels(bool inputs)
        {
            UInt32 propsize;
            unsigned result = 0;
            
            AudioObjectPropertyAddress theAddress =
            { kAudioDevicePropertyStreamConfiguration,
              inputs?kAudioDevicePropertyScopeInput:kAudioDevicePropertyScopeOutput,
                0 };
            
            THROW_ERROR(DeviceInitializationFailure,AudioObjectGetPropertyDataSize(caID,&theAddress,0,NULL,&propsize));
            vector<uint8_t> space(propsize);
            AudioBufferList *buflist = (AudioBufferList*)space.data();
            THROW_ERROR(DeviceInitializationFailure,AudioObjectGetPropertyData(caID,&theAddress,0,NULL,&propsize,buflist));
            for(unsigned i(0);i<buflist->mNumberBuffers;++i)
            {
                result+=buflist->mBuffers[i].mNumberChannels;
            }
            return result;
        }
        
        string GetName(bool input)
        {
            UInt32 propsize;
            
            AudioObjectPropertyAddress theAddress =
            { kAudioDevicePropertyDeviceName,
                input?kAudioDevicePropertyScopeInput:kAudioDevicePropertyScopeOutput,
                0 };
            
            THROW_ERROR(DeviceInitializationFailure,AudioObjectGetPropertyDataSize(caID,&theAddress,0,NULL,&propsize));
            vector<char> space(propsize);
            THROW_ERROR(DeviceInitializationFailure,
                        AudioObjectGetPropertyData(caID,&theAddress,0,NULL,&propsize,space.data()));
            return string(space.data(),space.data()+space.size());
        }
        
        unsigned numInputs, numOutputs;
        string devName;
        AudioStreamConfiguration Conform(AudioStreamConfiguration c) const
        {
            return c;
        }

    public:
        CoreAudioDevice(AudioDeviceID id)
        :caID(id),numInputs(CountChannels(true)),numOutputs(CountChannels(false)),
        devName(GetName(true)+"/"+GetName(false))
        {
        }
        
        
        const char* GetName() const {return devName.c_str();}
        unsigned GetNumInputs() const {return numInputs;}
        unsigned GetNumOutputs() const {return numOutputs;}
        const char *GetHostAPI() const {return "CoreAudio";}
        
        AudioStreamConfiguration DefaultMono() const {return Conform(AudioStreamConfiguration(44100).Input(0).Output(0));}
        AudioStreamConfiguration DefaultStereo() const {return Conform(AudioStreamConfiguration(44100).StereoInput(0).StereoOutput(0));}
        AudioStreamConfiguration DefaultAllChannels() const {return Conform(AudioStreamConfiguration(44100).Inputs(ChannelRange(0,numInputs)).Outputs(ChannelRange(0,numOutputs)));}
        
        void Resume() {}
        void Suspend() {}
        void Close() {}
        
        const AudioStreamConfiguration& Open(const AudioStreamConfiguration& c)
        {
            
            return c;
        }
        
        bool Supports(const AudioStreamConfiguration&) const {return false;}
    };
    
    
    class CoreaudioPublisher : public HostAPIPublisher{
        list<CoreAudioDevice> devices;
    public:
        void RegisterDevice(Session& PI, const CoreAudioDevice& dev)
        {
            devices.push_back(dev);
            PI.Register(&devices.back());
        }
        
        void Publish(Session& PADInstance, DeviceErrorDelegate& errorHandler)
        {
            UInt32 propsize(0);
            AudioObjectPropertyAddress theAddress = { kAudioHardwarePropertyDevices,
                                                      kAudioObjectPropertyScopeGlobal,
                                                      kAudioObjectPropertyElementMaster };
            
            THROW_ERROR(DeviceInitializationFailure,AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&theAddress,0,NULL,&propsize));
            
            vector<AudioDeviceID> devids(propsize / sizeof(AudioDeviceID));
            THROW_ERROR(DeviceInitializationFailure,AudioObjectGetPropertyData(kAudioObjectSystemObject, &theAddress, 0, NULL, &propsize,devids.data()));
            
            for(auto dev : devids)
            {
                try{
                    RegisterDevice(PADInstance,CoreAudioDevice(dev));
                }
                catch(SoftError se)
                {
                    errorHandler.Catch(se);
                }
                catch(HardError he)
                {
                    errorHandler.Catch(he);
                }
            }
        }
        const char *GetName() const {return "CoreAudio";}
    } publisher;
}
