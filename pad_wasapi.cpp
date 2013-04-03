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
#include <mutex>
#include <memory>
#include <algorithm>
#include "Avrt.h"
#include <cmath>

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);

void CopyWavFormat(WAVEFORMATEXTENSIBLE& dest, const WAVEFORMATEX* const src)
{
    if (src->wFormatTag==WAVE_FORMAT_EXTENSIBLE)
        memcpy(&dest,src,sizeof(WAVEFORMATEXTENSIBLE));
    else
        memcpy(&dest,src,sizeof(WAVEFORMATEX));
}

inline int simple_round(double x)
{
    return x+0.5;
}

int RefTimeToSamples(const REFERENCE_TIME& v, const double sr)
{
    return simple_round(sr * ((double) v) * 0.0000001);
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
    struct WasapiEndPoint
    {
        WasapiEndPoint() : m_numChannels(0), m_isDefault(0) {}
        WasapiEndPoint(const PadComSmartPointer<IMMDevice>& ep,unsigned numch, unsigned isDefault, const std::string& name) :
            m_numChannels(numch), m_name(name), m_Endpoint(ep), m_isDefault(isDefault)
        {
        }
        PadComSmartPointer<IAudioClient> m_AudioClient;
        PadComSmartPointer<IAudioRenderClient> m_AudioRenderClient;
        PadComSmartPointer<IMMDevice> m_Endpoint;
        unsigned m_numChannels;
        std::string m_name;
        unsigned m_isDefault;
    };

    enum WasapiState
    {
        WASS_Idle,
        WASS_Open,
        WASS_Playing,
        WASS_Closed
    };

    WasapiDevice() : m_deviceName("Invalid"),
        m_numInputs(0), m_numOutputs(0), m_audioThreadHandle(0), currentDelegate(0), m_currentState(WASS_Idle),
        m_threadShouldStop(false)
    {
    }
    ~WasapiDevice()
    {
        //cerr << "WasapiDevice dtor\n";
        if (m_audioThreadHandle!=0)
            Close();
    }

    const char *GetName() const { return m_deviceName.c_str(); }
    const char *GetHostAPI() const { return "WASAPI"; }
    void SetName(const std::string& n) { m_deviceName=n; }
    void AddInputEndPoint(const PadComSmartPointer<IMMDevice>& ep, unsigned numChannels, const std::string& name=std::string())
    {
        cerr << "PAD/WASAPI: Inputs are not yet supported\n";
        /*
        m_numInputs+=numChannels;
        WasapiEndPoint wep(ep, numChannels,name);
        m_inputEndPoints.push_back(wep);
        */
    }
    void AddOutputEndPoint(const PadComSmartPointer<IMMDevice>& ep,unsigned numChannels, bool isDefault,const std::string& name=std::string())
    {
        m_numOutputs+=numChannels;
        unsigned foo=1; if (isDefault==true) foo=0;
        WasapiEndPoint wep(ep, numChannels, foo, name);
        // I realize this is theoretically inefficient and not really elegant but I hate std::list so much I don't want to use it
        // just to get push_front and to avoid moving the couple of entries around in the std::vector which I prefer to use in the code
        m_outputEndPoints.push_back(wep);
        std::sort(m_outputEndPoints.begin(),m_outputEndPoints.end(),
                  [](const WasapiEndPoint& a,const WasapiEndPoint& b) { return a.m_isDefault<b.m_isDefault; });
    }
    unsigned GetNumInputs() const {return m_numInputs;}
    unsigned GetNumOutputs() const {return m_numOutputs;}
    void InitDefaultStreamConfigurations()
    {
        int defSr=44100;
        if (m_supportedSampleRates.size()>0)
            defSr=m_supportedSampleRates.at(0);
        if (m_numOutputs >= 1)
        {
            m_defaultMono = AudioStreamConfiguration(defSr,true);
            m_defaultMono.AddDeviceOutputs(Channel(0));
            if (m_numInputs > 0) m_defaultMono.AddDeviceInputs(Channel(0));
        }
        else m_defaultMono.SetValid(false);

        if (m_numOutputs >= 2)
        {
            m_defaultStereo = AudioStreamConfiguration(defSr,true);
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
        cerr << "Supports() not implemented\n";
        return false;
    }

    virtual const AudioStreamConfiguration& Open(const AudioStreamConfiguration& conf)
    {
        cerr << "Open\n";
        if (m_currentState==WASS_Playing)
        {
            cerr << "Open called when already playing\n";
            return currentConfiguration;
        }
        const unsigned endPointToActivate=0;
        //cout << "Wasapi::Open, thread id "<<GetCurrentThreadId()<<"\n";
        m_glitchCounter=0;
        currentDelegate = &conf.GetAudioDelegate();
        currentConfiguration=conf;
        if (m_currentState==WASS_Closed || m_currentState==WASS_Idle)
        {
            HRESULT hr=0;
            if (m_outputEndPoints.size()>0)
            {
                if (currentConfiguration.GetNumStreamOutputs()>m_outputEndPoints.at(endPointToActivate).m_numChannels)
                {
                    currentConfiguration.SetDeviceChannelLimits(0,m_outputEndPoints.at(endPointToActivate).m_numChannels);
                }
                PadComSmartPointer<IAudioClient> theClient;
                hr = m_outputEndPoints.at(endPointToActivate).m_Endpoint->Activate(
                            __uuidof (IAudioClient), CLSCTX_ALL,nullptr, (void**)theClient.NullAndGetPtrAddress());
                if (CheckHResult(hr,"PAD/WASAPI : Activate output audioclient")==true)
                {
                    if (theClient)
                    {
                        WAVEFORMATEX *pMixformat=nullptr;
                        hr = theClient->GetMixFormat(&pMixformat);
                        hr = theClient->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,150000,0,pMixformat, NULL);
                        if (CheckHResult(hr,"PAD/WASAPI : Initialize audio render client")==true)
                        {
                            UINT32 nFramesInBuffer=0;
                            hr = theClient->GetBufferSize(&nFramesInBuffer);
                            if (nFramesInBuffer>7 && nFramesInBuffer<32769)
                            {
                                m_delegateBuffer.resize(nFramesInBuffer*m_numOutputs);
                                currentConfiguration.SetBufferSize(nFramesInBuffer);
                                PadComSmartPointer<IAudioRenderClient> theAudioRenderClient;
                                hr = theClient->GetService(__uuidof(IAudioRenderClient),(void**)theAudioRenderClient.NullAndGetPtrAddress());
                                m_outputEndPoints[endPointToActivate].m_AudioClient=theClient;
                                m_outputEndPoints[endPointToActivate].m_AudioRenderClient=theAudioRenderClient;
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
        if (m_currentState==WASS_Open)
            InitAudioThread();
        if (m_currentState==WASS_Open && conf.HasSuspendOnStartup() == false) Run();
        return currentConfiguration;
    }
    void InitAudioThread()
    {
        if (m_audioThreadHandle==0)
        {
            m_threadShouldStop=false;
            m_audioThreadHandle=CreateThread(NULL, 0,WasapiThreadFunction,(void*)this, 0,0);
            if (m_audioThreadHandle==0)
            {
                cerr << "PAD/WASAPI : Could not create audio thread :C\n";
            }
        }
    }

    void Run()
    {
        if (m_currentState!=WASS_Open)
        {
            cerr << "PAD/WASAPI : Run called when device not open\n";
            return;
        }
        Resume();
    }

    void Resume()
    {
        //cout << "Pad/Wasapi : Resume()\n";
        if (m_audioThreadHandle)
        {
            m_currentState=WASS_Playing;
            //ResumeThread(m_audioThreadHandle);
        }
    }

    void Suspend()
    {
        if (m_currentState!=WASS_Playing)
        {
            cerr << "PAD/WASAPI : Suspend() called when not playing\n";
            return;
        }
        m_currentState=WASS_Idle;
        cout  << "Pad/Wasapi : Suspend()\n";
    }

    void Close()
    {
        if (m_currentState==WASS_Closed)
        {
            cout << "PAD/WASAPI : Close called when already closed\n";
            return;
        }
        cout << "Pad/Wasapi close, waiting for thread to stop...\n";
        m_currentState=WASS_Idle;
        m_threadShouldStop=true;
        if (WaitForSingleObject(m_audioThreadHandle,1000)==WAIT_TIMEOUT)
            cout << "Pad/Wasapi close, audio thread timed out when stopping\n";
        m_currentState=WASS_Closed;
        CloseHandle(m_audioThreadHandle);
        m_audioThreadHandle=0;
        //cout << "Pad/Wasapi close, thread was stopped\n";
    }
    void EnableMultiMediaThreadPriority(bool proAudio=false)
    {
        HMODULE hModule=LoadLibrary("avrt.dll");
        if (hModule)
        {
            HANDLE (WINAPI *ptrAvSetMmThreadCharacteristics)(LPCTSTR,LPDWORD);
            *((void **)&ptrAvSetMmThreadCharacteristics)=(void*)GetProcAddress(hModule,"AvSetMmThreadCharacteristicsA");
            BOOL (WINAPI *ptrAvSetMmThreadPriority)(HANDLE,AVRT_PRIORITY);
            *((void **)&ptrAvSetMmThreadPriority)=(void*)GetProcAddress(hModule,"AvSetMmThreadPriority");
            if (ptrAvSetMmThreadCharacteristics!=0 && ptrAvSetMmThreadPriority!=0)
            {
                DWORD foo=0;
                HANDLE h = 0;
                if (proAudio==false)
                    h=ptrAvSetMmThreadCharacteristics ("Audio", &foo);
                else h=ptrAvSetMmThreadCharacteristics ("Pro Audio", &foo);
                if (h != 0)
                {
                    BOOL result=ptrAvSetMmThreadPriority (h, AVRT_PRIORITY_NORMAL);
                    if (result==FALSE)
                        cerr << "AvSetMmThreadPriority failed\n";
                }
                else cerr << "Task wasn't returned from AvSetMmThreadCharacteristics\n";
            } else cerr << "Could not resolve functions from avrt.dll\n";
        } else cerr << "Could not load avrt.dll\n";
    }

    AudioCallbackDelegate* currentDelegate;
    AudioStreamConfiguration currentConfiguration;
    //PadComSmartPointer<IAudioClient> m_outputAudioClient;
    //PadComSmartPointer<IAudioRenderClient> outputAudioRenderClient;
    vector<float> m_delegateBuffer;
    unsigned m_glitchCounter;
    vector<bool> m_enabledDeviceOutputs;
    WasapiState m_currentState;
    std::shared_ptr<std::mutex> m_mutex;
    vector<WasapiEndPoint> m_inputEndPoints;
    vector<WasapiEndPoint> m_outputEndPoints;
    bool m_threadShouldStop;
    std::vector<int> m_supportedSampleRates;
private:
    HANDLE m_audioThreadHandle;
    //vector<PadComSmartPointer<IMMDevice>> m_inputEndpoints;
    //vector<PadComSmartPointer<IMMDevice>> m_outputEndpoints;
    string m_deviceName;
    unsigned m_numInputs;
    unsigned m_numOutputs;
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
        using namespace chrono;
        high_resolution_clock::time_point t1 = high_resolution_clock::now();
        CoInitialize(0);
        HRESULT hr = S_OK;
        PadComSmartPointer<IMMDeviceEnumerator> enumerator;
        hr=enumerator.CoCreateInstance(CLSID_MMDeviceEnumerator,CLSCTX_ALL);
        if (CheckHResult(hr,"PAD/WASAPI : Could not create device enumerator")==false) return;
        PadComSmartPointer<IMMDeviceCollection> collection;
        PadComSmartPointer<IMMDevice> endpoint;
        PadComSmartPointer<IMMDevice> defaultEndpoint;
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
            return;
        }
        std::string defaultDevId;
        if (CheckHResult(enumerator->GetDefaultAudioEndpoint(eRender,eMultimedia,defaultEndpoint.NullAndGetPtrAddress()),"PAD/WASAPI : Could not get default endpoint device"))
        {
            WCHAR* deviceId=nullptr;
            defaultEndpoint->GetId(&deviceId);
            defaultDevId=WideCharToStdString(deviceId);
            CoTaskMemFree (deviceId);
        }
        std::vector<int> sampleRatesToTest;
        sampleRatesToTest.push_back(44100); sampleRatesToTest.push_back(48000); sampleRatesToTest.push_back(88200); sampleRatesToTest.push_back(96000);
        sampleRatesToTest.push_back(176400); sampleRatesToTest.push_back(192000);
        for (unsigned i=0;i<count;i++)
        {
            bool isDefaultDevice=false;
            hr = collection->Item(i, endpoint.NullAndGetPtrAddress());
            if (CheckHResult(hr,"PAD/WASAPI : Could not get endpoint collection item")==false) continue;
            WCHAR* deviceId=nullptr;
            endpoint->GetId(&deviceId);
            std::string curDevId=WideCharToStdString(deviceId);
            CoTaskMemFree(deviceId);
            if (curDevId==defaultDevId)
                isDefaultDevice=true;
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
            std::string endPointNameString=WideCharToStdString(endPointName()->pwszVal);
            EDataFlow audioDirection=getAudioDirection(endpoint);
            PadComSmartPointer<IAudioClient> tempClient;
            hr = endpoint->Activate(__uuidof (IAudioClient), CLSCTX_ALL,nullptr, (void**)tempClient.NullAndGetPtrAddress());
            if (tempClient==nullptr)
            {
                cerr << "pad : wasapi : could not create temp client for " << i << " to get channel counts and shit\n";
                continue;
            }
            REFERENCE_TIME defPer, minPer;
            if (CheckHResult(tempClient->GetDevicePeriod(&defPer,&minPer))==false)
            {
                cerr << "PAD/WASAPI : could not get device default/min period for " << endPointNameString << "\n";
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
            int defaultSr=format.Format.nSamplesPerSec;
            wasapiMap[adapterNameString].m_supportedSampleRates.push_back(defaultSr);
            //cerr << endPointNameString << " default sr is " << defaultSr << "\n";
            int minPeriodSamples=RefTimeToSamples(minPer,defaultSr);
            int defaultPeriodSamples=RefTimeToSamples(defPer,defaultSr);
            //cerr << endPointNameString << " minimum period is " << (double)minPer/10000 << " ms, default period is " << (double)defPer/10000 << " ms\n";
            //cerr << endPointNameString << " minimum buf size is " << minPeriodSamples << " , default buf size is " << defaultPeriodSamples << "\n";
            for (int samplerate : sampleRatesToTest)
            {
                if (samplerate==defaultSr)
                    continue;
                format.Format.nSamplesPerSec=(DWORD)samplerate;
                if (tempClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED,(WAVEFORMATEX*)&format,0)>=0)
                {
                    cerr << endPointNameString << " supports " << samplerate << " hz\n";
                    wasapiMap[adapterNameString].m_supportedSampleRates.push_back(samplerate);
                }
            }
            if (audioDirection==eRender)
            {
                wasapiMap[adapterNameString].AddOutputEndPoint(endpoint,format.Format.nChannels,isDefaultDevice, endPointNameString);
            }
            else if (audioDirection==eCapture)
            {
                wasapiMap[adapterNameString].AddInputEndPoint(endpoint,format.Format.nChannels,endPointNameString);
            }
        }
        for(std::map<std::string,WasapiDevice>::iterator iter=wasapiMap.begin(); iter!=wasapiMap.end(); ++iter)
        {
            iter->second.InitDefaultStreamConfigurations();
            PADInstance.Register(&iter->second);
        }
        high_resolution_clock::time_point t2 = high_resolution_clock::now();
        duration<double> time_span = duration_cast<duration<double>>(t2 - t1);
        std::cerr << "Enumerating WASAPI devices took " << time_span.count()*1000.0 << " ms\n";
    }
} publisher;

DWORD WINAPI WasapiThreadFunction(LPVOID params)
{
    WasapiDevice* dev=(WasapiDevice*)params;
    if (dev->m_outputEndPoints.size()==0)
    {
        return 0;
    }
    CheckHResult(CoInitialize(0),"Wasapi audio thread could not init COM");
    dev->EnableMultiMediaThreadPriority(true);
    //cout << "*** Starting wasapi audio thread "<<GetCurrentThreadId()<<"\n";
    std::vector<HANDLE> wantDataEvents;
    // 1 is used as a hack until rendering multiple endpoints can be properly tested
    const unsigned numOutputEndPoints=1; //dev->m_outputEndPoints.size();
    wantDataEvents.resize(numOutputEndPoints);
    HRESULT hr=0;
    for (unsigned int i=0;i<numOutputEndPoints;i++)
    {
        wantDataEvents[i]=CreateEvent(NULL,FALSE,FALSE,NULL);
        dev->m_outputEndPoints.at(i).m_AudioClient->SetEventHandle(wantDataEvents[i]);
        hr=dev->m_outputEndPoints.at(i).m_AudioClient->Start();
        CheckHResult(hr,"Wasapi TestFunction Start audio");
    }
    int counter=0;
    int nFramesInBuffer=dev->currentConfiguration.GetBufferSize();
    BYTE* data=0;
    while (dev->m_threadShouldStop==false)
    {
        while (dev->m_currentState==WasapiDevice::WASS_Playing)
        {
            const AudioStreamConfiguration curConf=dev->currentConfiguration;
            if (WaitForMultipleObjects(numOutputEndPoints,wantDataEvents.data(),TRUE,1000)==WAIT_OBJECT_0)
            {
                for (unsigned ep=0;ep<numOutputEndPoints;ep++)
                {
                    UINT32 nFramesOfPadding;
                    hr = dev->m_outputEndPoints.at(ep).m_AudioClient->GetCurrentPadding(&nFramesOfPadding);
                    if (nFramesOfPadding==nFramesInBuffer)
                    {
                        dev->m_glitchCounter++;
                    }
                    hr = dev->m_outputEndPoints.at(ep).m_AudioRenderClient->GetBuffer(nFramesInBuffer - nFramesOfPadding, &data);
                    if (hr>=0 && data!=nullptr)
                    {
                        dev->currentDelegate->Process(0,curConf,0,dev->m_delegateBuffer.data(),nFramesInBuffer - nFramesOfPadding);
                        float* pf=(float*)data;
                        unsigned numStreamChans=curConf.GetNumStreamOutputs();
                        unsigned numEndpointChans=dev->m_outputEndPoints.at(ep).m_numChannels;
                        unsigned k=0;
                        for (unsigned i=0;i<numEndpointChans;i++)
                        {
                            if (dev->m_enabledDeviceOutputs[i]==true)
                            {
                                for (unsigned j=0;j<nFramesInBuffer-nFramesOfPadding;j++)
                                {
                                    pf[j*numEndpointChans+i]=dev->m_delegateBuffer[j*numStreamChans+k];
                                }
                                k++;
                            } else
                            {
                                for (unsigned j=0;j<nFramesInBuffer-nFramesOfPadding;j++)
                                {
                                    pf[j*numEndpointChans+i]=0.0;
                                }
                            }
                        }
                        hr = dev->m_outputEndPoints.at(ep).m_AudioRenderClient->ReleaseBuffer(nFramesInBuffer - nFramesOfPadding, 0);
                    }
                }
                counter++;
            } else
            {
                cerr << "PAD/WASAPI : Audio thread had to wait too long for data, ending thread\n";
                break;
            }
        }
        //cerr << "WASAPI thread polling for playback status change...\n";
        Sleep(1);
    }
    for (unsigned i=0;i<numOutputEndPoints;i++)
    {
        hr = dev->m_outputEndPoints.at(i).m_AudioClient->Stop();
        CloseHandle(wantDataEvents[i]);
    }
    CoUninitialize();
    //if (dev->m_glitchCounter>0)
        cerr << "ended wasapi audio thread. "<<dev->m_glitchCounter<< " glitches detected\n";
    return 0;
}

}

#ifdef WIN32
extern "C" const char* LinkWasapi() {return publisher.GetName();}
#pragma comment(linker,"/include:LinkWasapi")
#endif
