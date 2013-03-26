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
#include <thread>
#include <memory>

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

class WasapiDevice;

DWORD WINAPI WasapiThreadFunction(LPVOID params);
class WasapiDevice : public AudioDevice
{
public:
    WasapiDevice() : deviceName("Invalid"), numInputs(0), numOutputs(0), audioThreadHandle(0), currentDelegate(0), m_stopThread(false)
    {
    }
    ~WasapiDevice()
    {
        if (audioThreadHandle)
            CloseHandle(audioThreadHandle);
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

    virtual const AudioStreamConfiguration& Open(const AudioStreamConfiguration& conf)
    {
        cout << "Wasapi::Open, thread id "<<GetCurrentThreadId()<<"\n";
        glitchCounter=0;
        m_stopThread=false;
        currentDelegate = &conf.GetAudioDelegate();
        currentConfiguration=conf;
        HRESULT hr=0;
        if (outputEndpoints.size()>0)
        {
            hr = outputEndpoints.at(0)->Activate(__uuidof (IAudioClient), CLSCTX_ALL,nullptr, (void**)outputAudioClient.NullAndGetPtrAddress());
            if (CheckHResult(hr,"PAD/WASAPI : Activate output audioclient")==true)
            {
                if (outputAudioClient)
                {
                    WAVEFORMATEX *pwfx;
                    hr = outputAudioClient->GetMixFormat(&pwfx);
                    hr = outputAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,0, 0, pwfx, NULL);
                    UINT32 nFramesInBuffer;
                    hr = outputAudioClient->GetBufferSize(&nFramesInBuffer);
                    if (nFramesInBuffer>8 && nFramesInBuffer<32768)
                    {
                        delegateBuffer.resize(nFramesInBuffer*numOutputs);
                        currentConfiguration.SetBufferSize(nFramesInBuffer);
                        hr = outputAudioClient->GetService(__uuidof(IAudioRenderClient),(void**)outputAudioRenderClient.NullAndGetPtrAddress());
                    } else cerr << "PAD/WASAPI : Output audio client has an unusual buffer size "<<nFramesInBuffer<<"\n";

                } else cerr << "PAD/WASAPI : Could not initialize IAudioClient\n";
            }
        }
        if (conf.HasSuspendOnStartup() == false) Run();
        return currentConfiguration;
    }
    void Run()
    {
        if (audioThreadHandle==0)
        {
            cout << "creating wasapi audio thread...\n";
            audioThreadHandle=CreateThread(NULL, 0,WasapiThreadFunction, (void*)this, CREATE_SUSPENDED,NULL);
            if (!audioThreadHandle)
                cout << "it failed :C\n";
        }
        m_stopThread=false;
        Resume();
    }

    virtual void Resume()
    {
        cout << "Pad/Wasapi : Resume()\n";
        m_stopThread=false;
        if (audioThreadHandle)
        {
            ResumeThread(audioThreadHandle);
        }
    }
    virtual void Suspend()
    {
        m_stopThread=true;
        cout  << "Pad/Wasapi : Suspend()\n";
        if (audioThreadHandle)
        {
            WaitForSingleObject(audioThreadHandle,1000);
        }
    }

    virtual void Close()
    {
        cout << "Pad/Wasapi close, waiting for thread to stop...\n";
        m_stopThread=true;
        if (WaitForSingleObject(audioThreadHandle,1000)==WAIT_TIMEOUT)
            cout << "Pad/Wasapi close, thread timed out when stopping\n";
        cout << "Pad/Wasapi close, thread was stopped\n";
    }
    AudioCallbackDelegate* currentDelegate;
    AudioStreamConfiguration currentConfiguration;
    PadComSmartPointer<IAudioClient> outputAudioClient;
    PadComSmartPointer<IAudioRenderClient> outputAudioRenderClient;
    bool m_stopThread;
    vector<float> delegateBuffer;
    unsigned glitchCounter;
private:
    HANDLE audioThreadHandle;
    vector<PadComSmartPointer<IMMDevice>> inputEndpoints;
    vector<PadComSmartPointer<IMMDevice>> outputEndpoints;
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
    if (CheckHResult(hr)==false)
        return flowDirection;
    hr=endPoint->GetDataFlow(&flowDirection);
    CheckHResult(hr);
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
        if (CheckHResult(hr,"PAD/WASAPI : Could not create device enumerator")==false) return;
        PadComSmartPointer<IMMDeviceCollection> collection;
        PadComSmartPointer<IMMDevice> endpoint;
        PadComSmartPointer<IPropertyStore> props;
        //LPWSTR pwszID = NULL;
        hr=enumerator->EnumAudioEndpoints(eAll,DEVICE_STATE_ACTIVE,collection.NullAndGetPtrAddress());
        if (CheckHResult(hr,"PAD/WASAPI : Could not enumerate audio endpoints")==false) return;
        UINT count;
        hr = collection->GetCount(&count);
        if (CheckHResult(hr,"PAD/WASAPI : Could not get endpoint collection count")==false) return;
        if (count == 0)
        {
            cerr << "pad : wasapi : No endpoints found\n";
        }
        for (unsigned i=0;i<count;i++)
        {
            hr = collection->Item(i, endpoint.NullAndGetPtrAddress());
            if (CheckHResult(hr,"PAD/WASAPI : Could not get endpoint collection item")==false) continue;
            hr = endpoint->OpenPropertyStore(STGM_READ, props.NullAndGetPtrAddress());
            if (CheckHResult(hr,"PAD/WASAPI : Could not open endpoint property store")==false) continue;
            MyPropVariant adapterName;
            hr = props->GetValue(PKEY_DeviceInterface_FriendlyName, adapterName());
            if (CheckHResult(hr,"PAD/WASAPI : Could not get endpoint adapter name")==false) continue;
            std::string adapterNameString=WideCharToStdString(adapterName()->pwszVal);
            wasapiMap[adapterNameString].SetName(adapterNameString);
            MyPropVariant endPointName;
            hr = props->GetValue(PKEY_Device_FriendlyName, endPointName());
            if (CheckHResult(hr,"PAD/WASAPI : Could not get endpoint name")==false) continue;
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
            if (CheckHResult(hr,"PAD/WASAPI : Could not get mix format")==false) continue;
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

DWORD WINAPI WasapiThreadFunction(LPVOID params)
{
    WasapiDevice* dev=(WasapiDevice*)params;
    cout << "starting wasapi audio thread "<<GetCurrentThreadId()<<"\n";
    HANDLE hWantData=CreateEvent(NULL, FALSE, FALSE, NULL);
    dev->outputAudioClient->SetEventHandle(hWantData);
    HRESULT hr=dev->outputAudioClient->Start();
    CheckHResult(hr,"Wasapi TestFunction Start audio");
    int counter=0;
    int nFramesInBuffer=dev->currentConfiguration.GetBufferSize();
    BYTE* data=0;
    while (dev->m_stopThread==false)
    {
        const AudioStreamConfiguration curConf=dev->currentConfiguration;
        if (WaitForSingleObject(hWantData,1000)==WAIT_OBJECT_0)
        {
            UINT32 nFramesOfPadding;
            hr = dev->outputAudioClient->GetCurrentPadding(&nFramesOfPadding);
            if (nFramesOfPadding==nFramesInBuffer)
            {
                dev->glitchCounter++;
            }
            hr = dev->outputAudioRenderClient->GetBuffer(nFramesInBuffer - nFramesOfPadding, &data);
            if (hr>=0 && data!=nullptr)
            {
                dev->currentDelegate->Process(0,curConf,0,dev->delegateBuffer.data(),nFramesInBuffer - nFramesOfPadding);
                float* pf=(float*)data;
                unsigned numStreamChans=curConf.GetNumStreamOutputs();
                unsigned numDeviceChans=dev->GetNumOutputs();
                unsigned k=0;
                for (unsigned i=0;i<numDeviceChans;i++)
                {
                    if (curConf.IsOutputEnabled(i)==true)
                    {
                        //cout << "chan " << i << " audio " << counter << "\n";
                        for (unsigned j=0;j<nFramesInBuffer-nFramesOfPadding;j++)
                        {
                            pf[j*numDeviceChans+i]=dev->delegateBuffer[j*numStreamChans+k];
                        }
                        k++;
                    } else
                    {
                        //cout << "chan " << i << " tacet " << counter << "\n";
                        for (unsigned j=0;j<nFramesInBuffer-nFramesOfPadding;j++)
                        {
                            pf[j*numDeviceChans+i]=0.0;
                        }
                    }
                }
                hr = dev->outputAudioRenderClient->ReleaseBuffer(nFramesInBuffer - nFramesOfPadding, 0);
            }
            counter++;
        }
    }
    dev->outputAudioClient->Stop();
    CloseHandle(hWantData);
    cout << "ended wasapi audio thread. "<<dev->glitchCounter<< " glitches detected\n";
    return 0;
}
}
