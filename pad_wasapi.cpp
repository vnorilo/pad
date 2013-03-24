#include <iostream>
#include <string>
#include <list>
#include "utils/pad_com_smartpointer.h"
#include "winerror.h"
#include "Mmdeviceapi.h"
#include "Functiondiscoverykeys_devpkey.h"
#include "Audioclient.h"
#include "HostAPI.h"
#include "PAD.h"
#include <map>

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);

void CopyWavFormat(WAVEFORMATEXTENSIBLE& dest, const WAVEFORMATEX* const src)
{
    memcpy(&dest,src,src->wFormatTag==WAVE_FORMAT_EXTENSIBLE ? sizeof (WAVEFORMATEXTENSIBLE)
                                                                  : sizeof (WAVEFORMATEX));
}

namespace
{
using namespace std;
using namespace PAD;
class WasapiDevice : public AudioDevice
{
	AudioStreamConfiguration defaultMono, defaultStereo, defaultAll;
	AudioStreamConfiguration DefaultMono() const { return defaultMono; }
	AudioStreamConfiguration DefaultStereo() const { return defaultStereo; }
	AudioStreamConfiguration DefaultAllChannels() const { return defaultAll; }
public:
    WasapiDevice(unsigned i,double defaultRate,const string& name, unsigned inputs, unsigned outputs, int indIn,int indOut):
        deviceName(name),numInputs(inputs),numOutputs(outputs),index(i), indexOfInput(indIn), indexOfOutput(indOut)
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
        //defaultAll.AddDeviceInputs(ChannelRange(0,numInputs));
    }

    const char *GetName() const { return deviceName.c_str(); }
    const char *GetHostAPI() const { return "WASAPI"; }

    unsigned GetNumInputs() const {return numInputs;}
    unsigned GetNumOutputs() const {return numOutputs;}

    virtual bool Supports(const AudioStreamConfiguration&) const
    {
        return false;
    }

    virtual const AudioStreamConfiguration& Open(const AudioStreamConfiguration&)
    {
        static AudioStreamConfiguration kludge(44100);
        return kludge;
    }

    virtual void Resume() {}
    virtual void Suspend() {}

    virtual void Close() {}
private:
    string deviceName;
    unsigned numInputs;
    unsigned numOutputs;
    unsigned index;
    int indexOfInput;
    int indexOfOutput;
};

EDataFlow getAudioDirection (const PadComSmartPointer<IMMDevice>& device)
{
    EDataFlow flowDirection = eRender;
    PadComSmartPointer <IMMEndpoint> endPoint;
    HRESULT hr=device.QueryInterface(endPoint);
    if (hr<0)
    {
        cerr << "pad : wasapi : failure getting endpoint audio flow direction interface\n";
        return flowDirection;
    }
    hr=endPoint->GetDataFlow(&flowDirection);
    if (hr<0)
    {
        cerr << "pad : wasapi : failure getting endpoint audio flow direction\n";
    }
    return flowDirection;
}


struct WasapiPublisher : public HostAPIPublisher
{
    list<WasapiDevice> devices;
	const char *GetName() const {return "WASAPI";}

    void RegisterDevice(Session& PADInstance, WasapiDevice dev)
    {
		devices.push_back(dev);
		PADInstance.Register(&devices.back());
    }

	void Publish(Session& PADInstance, DeviceErrorDelegate& errorHandler)
	{
        std::map<std::string,std::vector<int>> adaptersMap; // string is adapter name, vector contains indexes of end points
        CoInitialize(0);
        HRESULT hr = S_OK;
        PadComSmartPointer<IMMDeviceEnumerator> enumerator;
        hr=enumerator.CoCreateInstance(CLSID_MMDeviceEnumerator,CLSCTX_ALL);
        if (hr<0)
        {
            cerr << "pad : wasapi : could not create device enumerator\n";
            return;
        }
        PadComSmartPointer<IMMDeviceCollection> collection;
        PadComSmartPointer<IMMDevice> endpoint;
        PadComSmartPointer<IPropertyStore> props;
        //LPWSTR pwszID = NULL;
        hr=enumerator->EnumAudioEndpoints(eAll,DEVICE_STATE_ACTIVE,collection.resetAndGetPointerAddress());
        if (hr<0)
        {
            cerr << "pad : wasapi : could not enumerate audio endpoints\n";
            return;
        }
        UINT count;
        hr = collection->GetCount(&count);
        if (hr<0)
        {
            cerr << "pad : wasapi : could not get endpoint count\n";
            return;
        }
        if (count == 0)
        {
            cerr << "pad : wasapi : No endpoints found\n";
        } //else cerr << count << " WASAPI endpoints found\n";
        for (unsigned i=0;i<count;i++)
        {
            hr = collection->Item(i, endpoint.resetAndGetPointerAddress());
            if (hr<0)
                continue;
            hr = endpoint->OpenPropertyStore(STGM_READ, props.resetAndGetPointerAddress());
            if (hr<0)
                continue;
            MyPropVariant adapterName;
            hr = props->GetValue(PKEY_DeviceInterface_FriendlyName, adapterName());
            if (hr<0)
            {
                cerr << "pad : wasapi : could not get adapter name for "<<i<<"\n";
                continue;
            }
            std::string adapterNameString=WideCharToStdString(adapterName()->pwszVal);
            adaptersMap[adapterNameString].push_back(i);
        }
        int wasapiDeviceCount=0;
        for( std::map<std::string,std::vector<int>>::iterator iter=adaptersMap.begin(); iter!=adaptersMap.end(); ++iter)
        {
            std::string name=iter->first;
            std::vector<int> indexes=iter->second;
            //cerr << "adapter " << name << " has end points\n";
            unsigned numInputs=0; unsigned numOutputs=0; int inputIndex=-1; int outputIndex=-1;
            for (unsigned i=0;i<indexes.size();i++)
            {
                //cerr << "\t" << indexes.at(i) << "\n";
                hr = collection->Item(indexes.at(i), endpoint.resetAndGetPointerAddress());
                if (hr<0)
                    continue;
                hr = endpoint->OpenPropertyStore(STGM_READ, props.resetAndGetPointerAddress());
                if (hr<0)
                    continue;
                MyPropVariant endPointName;
                hr = props->GetValue(PKEY_Device_FriendlyName, endPointName());
                if (hr<0)
                    continue;
                EDataFlow audioDirection=getAudioDirection(endpoint);
                PadComSmartPointer<IAudioClient> tempClient;
                hr = endpoint->Activate(__uuidof (IAudioClient), CLSCTX_ALL,nullptr, (void**)tempClient.resetAndGetPointerAddress());
                if (tempClient==nullptr)
                {
                    cerr << "pad : wasapi : could not create temp client for " << i << " to get channel counts and shit\n";
                    continue;
                }
                WAVEFORMATEX* mixFormat = nullptr;
                hr=tempClient->GetMixFormat(&mixFormat);
                if (hr<0)
                {
                    cerr << "could not get mix format\n";
                    continue;
                }
                WAVEFORMATEXTENSIBLE format;
                CopyWavFormat (format, mixFormat);
                CoTaskMemFree (mixFormat);
                if (audioDirection==eRender)
                {
                    numOutputs = format.Format.nChannels;
                    outputIndex=indexes.at(i);
                }
                else if (audioDirection==eCapture)
                {
                    numInputs= format.Format.nChannels;
                    inputIndex=indexes.at(i);
                }
                //std::string endPointNameString=WideCharToStdString(endPointName()->pwszVal);

            }
            if (numInputs>0 || numOutputs>0)
            {
                RegisterDevice(PADInstance,WasapiDevice(wasapiDeviceCount,44100.0,name,numInputs,numOutputs,inputIndex,outputIndex));
            }
            wasapiDeviceCount++;
        }

    }
} publisher;
}
