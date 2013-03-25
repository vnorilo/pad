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
public:
    WasapiDevice() : deviceName("Invalid"), numInputs(0), numOutputs(0)
    {

    }
    WasapiDevice(unsigned i,double defaultRate,const string& name, unsigned inputs, unsigned outputs):
        deviceName(name),numInputs(inputs),numOutputs(outputs),index(i)
    {

    }

    const char *GetName() const { return deviceName.c_str(); }
    const char *GetHostAPI() const { return "WASAPI"; }
    void SetName(const std::string& n) { deviceName=n; }
    void AddInputEndPoint(const PadComSmartPointer<IMMDevice>& ep, unsigned numChannels)
    {
        numInputs+=numChannels;
        inputEndpoints.push_back(ep);
    }
    void AddOutputEndPoint(const PadComSmartPointer<IMMDevice>& ep,unsigned numChannels)
    {
        numOutputs+=numChannels;
        outputEndpoints.push_back(ep);
    }
    unsigned GetNumInputs() const {return numInputs;}
    unsigned GetNumOutputs() const {return numOutputs;}
    void InitDefaultStreamConfigurations(double defaultRate)
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
    list<PadComSmartPointer<IMMDevice>> inputEndpoints;
    list<PadComSmartPointer<IMMDevice>> outputEndpoints;
    string deviceName;
    unsigned numInputs;
    unsigned numOutputs;
    unsigned index;
    AudioStreamConfiguration defaultMono, defaultStereo, defaultAll;
    AudioStreamConfiguration DefaultMono() const { return defaultMono; }
    AudioStreamConfiguration DefaultStereo() const { return defaultStereo; }
    AudioStreamConfiguration DefaultAllChannels() const { return defaultAll; }
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
    std::map<std::string,WasapiDevice> wasapiMap;
	const char *GetName() const {return "WASAPI";}
    void Publish(Session& PADInstance, DeviceErrorDelegate& errorHandler)
	{
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
        hr=enumerator->EnumAudioEndpoints(eAll,DEVICE_STATE_ACTIVE,collection.NullAndGetPtrAddress());
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
            hr = collection->Item(i, endpoint.NullAndGetPtrAddress());
            if (hr<0)
                continue;
            hr = endpoint->OpenPropertyStore(STGM_READ, props.NullAndGetPtrAddress());
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
            wasapiMap[adapterNameString].SetName(adapterNameString);
            MyPropVariant endPointName;
            hr = props->GetValue(PKEY_Device_FriendlyName, endPointName());
            if (hr<0)
                continue;
            EDataFlow audioDirection=getAudioDirection(endpoint);
            PadComSmartPointer<IAudioClient> tempClient;
            hr = endpoint->Activate(__uuidof (IAudioClient), CLSCTX_ALL,nullptr, (void**)tempClient.NullAndGetPtrAddress());
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
                wasapiMap[adapterNameString].AddOutputEndPoint(endpoint,format.Format.nChannels);
            }
            else if (audioDirection==eCapture)
            {
                wasapiMap[adapterNameString].AddInputEndPoint(endpoint,format.Format.nChannels);
            }
        }
        for( std::map<std::string,WasapiDevice>::iterator iter=wasapiMap.begin(); iter!=wasapiMap.end(); ++iter)
        {
            iter->second.InitDefaultStreamConfigurations(44100.0);
            PADInstance.Register(&iter->second);
        }
    }
} publisher;
}
