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
    enum WasapiState
    {
        WASS_Idle,
        WASS_Open,
        WASS_Playing,
        WASS_Closed
    };

    WasapiDevice() : m_deviceName("Invalid"),
        m_numInputs(0), m_numOutputs(0), m_audioThreadHandle(0), currentDelegate(0), m_currentState(WASS_Idle)
    {
    }
    ~WasapiDevice()
    {
        if (m_audioThreadHandle)
            CloseHandle(m_audioThreadHandle);
    }

    const char *GetName() const { return m_deviceName.c_str(); }
    const char *GetHostAPI() const { return "WASAPI"; }
    void SetName(const std::string& n) { m_deviceName=n; }
    void AddInputEndPoint(const PadComSmartPointer<IMMDevice>& ep, unsigned numChannels)
    {
        m_numInputs+=numChannels;
        m_inputEndpoints.push_back(ep);
    }
    void AddOutputEndPoint(const PadComSmartPointer<IMMDevice>& ep,unsigned numChannels)
    {
        m_numOutputs+=numChannels;
        m_outputEndpoints.push_back(ep);
    }
    unsigned GetNumInputs() const {return m_numInputs;}
    unsigned GetNumOutputs() const {return m_numOutputs;}
    void InitDefaultStreamConfigurations(double defaultRate)
    {
        if (m_numOutputs >= 1)
        {
            m_defaultMono = AudioStreamConfiguration(defaultRate,true);
            m_defaultMono.AddDeviceOutputs(Channel(0));
            if (m_numInputs > 0) m_defaultMono.AddDeviceInputs(Channel(0));
        }
        else m_defaultMono.SetValid(false);

        if (m_numOutputs >= 2)
        {
            m_defaultStereo = AudioStreamConfiguration(defaultRate,true);
            m_defaultStereo.AddDeviceOutputs(ChannelRange(0,2));
            if (m_numInputs > 0)
                m_defaultStereo.AddDeviceInputs(ChannelRange(0,min(m_numInputs,2u)));
        }
        else m_defaultStereo.SetValid(false);

        m_defaultAll = AudioStreamConfiguration(44100,true);
        m_defaultAll.AddDeviceOutputs(ChannelRange(0,m_numOutputs));
        m_defaultAll.AddDeviceInputs(ChannelRange(0,m_numInputs));
    }

    virtual bool Supports(const AudioStreamConfiguration&) const
    {
        return false;
    }

    virtual const AudioStreamConfiguration& Open(const AudioStreamConfiguration& conf)
    {
        if (m_currentState==WASS_Playing)
        {
            cout << "PAD/WASAPI : Open called when already playing!\n";
            return currentConfiguration;
        }
        cout << "Wasapi::Open, thread id "<<GetCurrentThreadId()<<"\n";
        m_glitchCounter=0;
        //m_stopThread=false;
        currentDelegate = &conf.GetAudioDelegate();
        currentConfiguration=conf;
        if (m_currentState==WASS_Closed || m_currentState==WASS_Idle)
        {
            HRESULT hr=0;
            if (m_outputEndpoints.size()>0)
            {
                hr = m_outputEndpoints.at(0)->Activate(__uuidof (IAudioClient), CLSCTX_ALL,nullptr, (void**)outputAudioClient.NullAndGetPtrAddress());
                if (CheckHResult(hr,"PAD/WASAPI : Activate output audioclient")==true)
                {
                    if (outputAudioClient)
                    {
                        WAVEFORMATEX *pwfx;
                        hr = outputAudioClient->GetMixFormat(&pwfx);
                        hr = outputAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,0,0,pwfx, NULL);
                        if (CheckHResult(hr,"PAD/WASAPI : Initialize audio render client")==true)
                        {
                            UINT32 nFramesInBuffer;
                            hr = outputAudioClient->GetBufferSize(&nFramesInBuffer);
                            if (nFramesInBuffer>8 && nFramesInBuffer<32768)
                            {
                                m_delegateBuffer.resize(nFramesInBuffer*m_numOutputs);
                                currentConfiguration.SetBufferSize(nFramesInBuffer);
                                hr = outputAudioClient->GetService(__uuidof(IAudioRenderClient),(void**)outputAudioRenderClient.NullAndGetPtrAddress());
                                m_enabledDeviceOutputs.resize(m_numOutputs);
                                for (unsigned i=0;i<m_numOutputs;i++)
                                {
                                    m_enabledDeviceOutputs[i]=currentConfiguration.IsOutputEnabled(i);
                                }
                                m_currentState=WASS_Open;
                            } else cerr << "PAD/WASAPI : Output audio client has an unusual buffer size "<<nFramesInBuffer<<"\n";
                        }

                    } else cerr << "PAD/WASAPI : Could not initialize IAudioClient\n";
                }
            }
        }
        if (m_currentState==WASS_Open && conf.HasSuspendOnStartup() == false) Run();
        return currentConfiguration;
    }
    void Run()
    {
        if (m_currentState!=WASS_Open)
        {
            cerr << "PAD/WASAPI : Run called when device not open\n";
            return;
        }
        if (m_audioThreadHandle==0)
        {
            cout << "creating wasapi audio thread...\n";
            m_audioThreadHandle=CreateThread(NULL, 0,WasapiThreadFunction,(void*)this, CREATE_SUSPENDED,NULL);
            if (!m_audioThreadHandle)
                cout << "PAD/WASAPI : Creating audio thread failed :C\n";
        }
        Resume();
    }

    virtual void Resume()
    {
        cout << "Pad/Wasapi : Resume()\n";
        if (m_audioThreadHandle)
        {
            m_currentState=WASS_Playing;
            ResumeThread(m_audioThreadHandle);
        }
    }
    virtual void Suspend()
    {
        if (m_currentState!=WASS_Playing)
        {
            cerr << "PAD/WASAPI : Suspend() called when not playing\n";
            return;
        }
        m_currentState=WASS_Idle;
        cout  << "Pad/Wasapi : Suspend()\n";
        if (m_audioThreadHandle)
        {
            WaitForSingleObject(m_audioThreadHandle,1000);
        }
    }

    virtual void Close()
    {
        cout << "Pad/Wasapi close, waiting for thread to stop...\n";
        m_currentState=WASS_Idle;
        if (WaitForSingleObject(m_audioThreadHandle,1000)==WAIT_TIMEOUT)
            cout << "Pad/Wasapi close, thread timed out when stopping\n";
        m_currentState=WASS_Closed;
        CloseHandle(m_audioThreadHandle);
        m_audioThreadHandle=0;
        cout << "Pad/Wasapi close, thread was stopped\n";
    }
    AudioCallbackDelegate* currentDelegate;
    AudioStreamConfiguration currentConfiguration;
    PadComSmartPointer<IAudioClient> outputAudioClient;
    PadComSmartPointer<IAudioRenderClient> outputAudioRenderClient;
    //bool m_stopThread;
    vector<float> m_delegateBuffer;
    unsigned m_glitchCounter;
    vector<bool> m_enabledDeviceOutputs;
    WasapiState m_currentState;
private:
    HANDLE m_audioThreadHandle;
    vector<PadComSmartPointer<IMMDevice>> m_inputEndpoints;
    vector<PadComSmartPointer<IMMDevice>> m_outputEndpoints;
    string m_deviceName;
    unsigned m_numInputs;
    unsigned m_numOutputs;
    unsigned m_index;
    AudioStreamConfiguration m_defaultMono, m_defaultStereo, m_defaultAll;
    AudioStreamConfiguration DefaultMono() const { return m_defaultMono; }
    AudioStreamConfiguration DefaultStereo() const { return m_defaultStereo; }
    AudioStreamConfiguration DefaultAllChannels() const { return m_defaultAll; }
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
            CopyWavFormat(format, mixFormat);
            //WAVE_FORMAT_IEEE_FLOAT
            //cout << "endpoint " << i << " format is " << mixFormat->wFormatTag << "\n";
            CoTaskMemFree(mixFormat);
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
    CheckHResult(CoInitialize(0),"Wasapi audio thread could not init COM");
    WasapiDevice* dev=(WasapiDevice*)params;
    cout << "*** Starting wasapi audio thread "<<GetCurrentThreadId()<<"\n";
    HANDLE hWantData=CreateEvent(NULL, FALSE, FALSE, NULL);
    dev->outputAudioClient->SetEventHandle(hWantData);
    HRESULT hr=dev->outputAudioClient->Start();
    CheckHResult(hr,"Wasapi TestFunction Start audio");
    int counter=0;
    int nFramesInBuffer=dev->currentConfiguration.GetBufferSize();
    BYTE* data=0;
    while (dev->m_currentState==WasapiDevice::WASS_Playing)
    {
        const AudioStreamConfiguration curConf=dev->currentConfiguration;
        if (WaitForSingleObject(hWantData,1000)==WAIT_OBJECT_0)
        {
            UINT32 nFramesOfPadding;
            hr = dev->outputAudioClient->GetCurrentPadding(&nFramesOfPadding);
            if (nFramesOfPadding==nFramesInBuffer)
            {
                dev->m_glitchCounter++;
            }
            hr = dev->outputAudioRenderClient->GetBuffer(nFramesInBuffer - nFramesOfPadding, &data);
            if (hr>=0 && data!=nullptr)
            {
                dev->currentDelegate->Process(0,curConf,0,dev->m_delegateBuffer.data(),nFramesInBuffer - nFramesOfPadding);
                float* pf=(float*)data;
                unsigned numStreamChans=curConf.GetNumStreamOutputs();
                unsigned numDeviceChans=dev->GetNumOutputs();
                unsigned k=0;
                for (unsigned i=0;i<numDeviceChans;i++)
                {
                    if (dev->m_enabledDeviceOutputs[i]==true)
                    {
                        for (unsigned j=0;j<nFramesInBuffer-nFramesOfPadding;j++)
                        {
                            pf[j*numDeviceChans+i]=dev->m_delegateBuffer[j*numStreamChans+k];
                        }
                        k++;
                    } else
                    {
                        for (unsigned j=0;j<nFramesInBuffer-nFramesOfPadding;j++)
                        {
                            pf[j*numDeviceChans+i]=0.0;
                        }
                    }
                }
                hr = dev->outputAudioRenderClient->ReleaseBuffer(nFramesInBuffer - nFramesOfPadding, 0);
            }
            counter++;
        } else
        {
            cerr << "PAD/WASAPI : Audio thread had to wait too long for data, ending thread\n";
            break;
        }
    }
    dev->outputAudioClient->Stop();
    CloseHandle(hWantData);
    CoUninitialize();
    cout << "ended wasapi audio thread. "<<dev->m_glitchCounter<< " glitches detected\n";
    return 0;
}
}
