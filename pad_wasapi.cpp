#include <iostream>
#include <string>
#include <list>
#include "utils/resourcemanagement.h"
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

#pragma comment(lib,"Ole32.lib")

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);

inline double scale_value_from_range_to_range(double v, double inputmin, double inputmax, double outputmin, double outputmax)
{
    double range1=inputmax-inputmin;
    double range2=outputmax-outputmin;
    return outputmin+range2/range1*v;
}

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
        WasapiEndPoint() : m_numChannels(0), m_sortID(0),m_isExclusiveMode(false) {}
        WasapiEndPoint(const PadComSmartPointer<IMMDevice>& ep,unsigned numch, unsigned sortID, const std::string& name, bool supportsExclusive) :
            m_numChannels(numch), m_name(name), m_Endpoint(ep), m_sortID(sortID),m_isExclusiveMode(supportsExclusive)
        {
        }
        PadComSmartPointer<IAudioClient> m_AudioClient;
        PadComSmartPointer<IAudioRenderClient> m_AudioRenderClient;
        PadComSmartPointer<IAudioCaptureClient> m_AudioCaptureClient;
        PadComSmartPointer<IMMDevice> m_Endpoint;
        unsigned m_numChannels;
        std::string m_name;
        unsigned m_sortID;
        bool m_isExclusiveMode;
    };

    enum WasapiState
    {
        WASS_Idle,
        WASS_Open,
        WASS_Playing,
        WASS_Closed
    };

    WasapiDevice() :
        m_numInputs(0), m_numOutputs(0), m_currentState(WASS_Idle) {}
    ~WasapiDevice()
    {
        //cerr << "WasapiDevice dtor\n";
        if (m_audioThreadHandle!=0)
            Close();
    }

    const char *GetName() const { return m_deviceName.c_str(); }
    const char *GetHostAPI() const { return "WASAPI"; }
    void SetName(const std::string& n) { m_deviceName=n; }
    void AddInputEndPoint(const PadComSmartPointer<IMMDevice>& ep, unsigned numChannels, unsigned sortID, bool canDoExclusive,const std::string& name=std::string())
    {
        m_numInputs+=numChannels;
        if (sortID==0)
            cerr << name << " is default input end point\n";
        WasapiEndPoint wep(ep, numChannels, sortID, name,canDoExclusive);
        // I realize this is theoretically inefficient and not really elegant but I hate std::list so much I don't want to use it
        // just to get push_front and to avoid moving the couple of entries around in the std::vector which I prefer to use in the code
        m_inputEndPoints.push_back(wep);
        std::sort(m_inputEndPoints.begin(),m_inputEndPoints.end(),
                  [](const WasapiEndPoint& a,const WasapiEndPoint& b) { return a.m_sortID<b.m_sortID; });
    }
    void AddOutputEndPoint(const PadComSmartPointer<IMMDevice>& ep,unsigned numChannels, unsigned sortID, bool canDoExclusive, const std::string& name=std::string())
    {
        m_numOutputs+=numChannels;
        if (sortID==0)
            cerr << name << " is default output end point\n";
        WasapiEndPoint wep(ep, numChannels, sortID, name,canDoExclusive);
        // I realize this is theoretically inefficient and not really elegant but I hate std::list so much I don't want to use it
        // just to get push_front and to avoid moving the couple of entries around in the std::vector which I prefer to use in the code
        m_outputEndPoints.push_back(wep);
        std::sort(m_outputEndPoints.begin(),m_outputEndPoints.end(),
                  [](const WasapiEndPoint& a,const WasapiEndPoint& b) { return a.m_sortID<b.m_sortID; });
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
        //cerr << "Open\n";
        if (m_currentState==WASS_Playing)
        {
            cerr << "Open called when already playing\n";
            return currentConfiguration;
        }
        const unsigned endPointToActivate=0;
        //cout << "Wasapi::Open, thread id "<<GetCurrentThreadId()<<"\n";
        m_outputGlitchCounter=0;
        currentDelegate = &conf.GetAudioDelegate();
        currentConfiguration=conf;
        currentConfiguration.SetValid(false);
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
                        if (m_outputEndPoints.at(endPointToActivate).m_isExclusiveMode==true)
                        {
                            REFERENCE_TIME hnsRequestedDuration = 0;
                            WAVEFORMATEXTENSIBLE format;
                            memset(&format,0,sizeof(WAVEFORMATEXTENSIBLE));
                            format.SubFormat=KSDATAFORMAT_SUBTYPE_PCM;
                            format.dwChannelMask=KSAUDIO_SPEAKER_STEREO;
                            format.Format.wFormatTag=WAVE_FORMAT_PCM;
                            format.Format.nChannels=2;
                            format.Format.cbSize=0; //sizeof(WAVEFORMATEXTENSIBLE);
                            format.Format.nSamplesPerSec=44100;
                            format.Format.wBitsPerSample=16;
                            format.Format.nBlockAlign=(format.Format.nChannels*format.Format.wBitsPerSample)/8;
                            format.Format.nAvgBytesPerSec=format.Format.nSamplesPerSec*format.Format.nBlockAlign;
                            hr = theClient->GetDevicePeriod(NULL, &hnsRequestedDuration);
                            cerr << "Device default period is " << hnsRequestedDuration << "\n";
                            hr = theClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                       hnsRequestedDuration,hnsRequestedDuration,(WAVEFORMATEX*)&format, NULL);
                            if (hr<0)
                                cerr << "PAD/WASAPI : Exclusive init error " << hr << "\n";
                        } else
                        {
                            hr = theClient->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,0,0,pMixformat, NULL);
                        }
                        if (CheckHResult(hr,"PAD/WASAPI : Initialize audio render client")==true)
                        {
                            UINT32 nFramesInBuffer=0;
                            hr = theClient->GetBufferSize(&nFramesInBuffer);
                            if (nFramesInBuffer>7 && nFramesInBuffer<32769)
                            {
                                m_delegateOutputBuffer.resize(nFramesInBuffer*m_numOutputs);
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
            if (m_inputEndPoints.size()>0 && m_currentState==WASS_Open)
            {
                //if (currentConfiguration.GetNumStreamInputs()>m_inputEndPoints.at(endPointToActivate).m_numChannels)
                //{
                //    currentConfiguration.SetDeviceChannelLimits(0,m_outputEndPoints.at(endPointToActivate).m_numChannels);
                //}
                PadComSmartPointer<IAudioClient> theClient;
                hr = m_inputEndPoints.at(endPointToActivate).m_Endpoint->Activate(
                            __uuidof (IAudioClient), CLSCTX_ALL,nullptr, (void**)theClient.NullAndGetPtrAddress());
                if (CheckHResult(hr,"PAD/WASAPI : Activate input audioclient")==true)
                {
                    if (theClient)
                    {
                        WAVEFORMATEX *pMixformat=nullptr;
                        hr = theClient->GetMixFormat(&pMixformat);
                        if (m_inputEndPoints.at(endPointToActivate).m_isExclusiveMode==true)
                        {
                            REFERENCE_TIME hnsRequestedDuration = 0;
                            WAVEFORMATEXTENSIBLE format;
                            memset(&format,0,sizeof(WAVEFORMATEXTENSIBLE));
                            format.SubFormat=KSDATAFORMAT_SUBTYPE_PCM;
                            format.dwChannelMask=KSAUDIO_SPEAKER_STEREO;
                            format.Format.wFormatTag=WAVE_FORMAT_PCM;
                            format.Format.nChannels=2;
                            format.Format.cbSize=0; //sizeof(WAVEFORMATEXTENSIBLE);
                            format.Format.nSamplesPerSec=44100;
                            format.Format.wBitsPerSample=16;
                            format.Format.nBlockAlign=(format.Format.nChannels*format.Format.wBitsPerSample)/8;
                            format.Format.nAvgBytesPerSec=format.Format.nSamplesPerSec*format.Format.nBlockAlign;
                            hr = theClient->GetDevicePeriod(NULL, &hnsRequestedDuration);
                            hr = theClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                       hnsRequestedDuration,hnsRequestedDuration,(WAVEFORMATEX*)&format, NULL);
                        } else
                        {
                            hr = theClient->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,0,0,pMixformat, NULL);
                        }
                        if (CheckHResult(hr,"PAD/WASAPI : Initialize audio capture client")==true)
                        {
                            UINT32 nFramesInBuffer=0;
                            hr = theClient->GetBufferSize(&nFramesInBuffer);
                            if (nFramesInBuffer>7 && nFramesInBuffer<32769)
                            {
                                m_delegateInputBuffer.resize(nFramesInBuffer*m_numInputs);
                                currentConfiguration.SetBufferSize(nFramesInBuffer);
                                PadComSmartPointer<IAudioCaptureClient> theAudioCaptureClient;
                                hr = theClient->GetService(__uuidof(IAudioCaptureClient),(void**)theAudioCaptureClient.NullAndGetPtrAddress());
                                m_inputEndPoints[endPointToActivate].m_AudioClient=theClient;
                                m_inputEndPoints[endPointToActivate].m_AudioCaptureClient=theAudioCaptureClient;
                                m_enabledDeviceInputs.resize(m_numInputs);
                                for (unsigned i=0;i<m_numOutputs;i++)
                                {
                                    m_enabledDeviceInputs[i]=currentConfiguration.IsInputEnabled(i);
                                }
                                m_currentState=WASS_Open;
                            } else cerr << "PAD/WASAPI : Input audio client has an unusual buffer size "<<nFramesInBuffer<<"\n";
                        }

                    } else cerr << "PAD/WASAPI : Could not initialize IAudioClient\n";
                }
            }
        }
        if (m_currentState!=WASS_Open)
            return currentConfiguration;
        if (currentDelegate)
            currentDelegate->AboutToBeginStream(currentConfiguration,*this);
        if (m_currentState==WASS_Open)
            InitAudioThread();
        if (m_currentState==WASS_Open && conf.HasSuspendOnStartup() == false) Run();
        currentConfiguration.SetValid(true);
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
        } else cerr << "PAD/WASAPI : Resume called when audio thread not initialized\n";
    }

    void Suspend()
    {
        if (m_currentState!=WASS_Playing)
        {
            cerr << "PAD/WASAPI : Suspend() called when not playing\n";
            return;
        }
        //cerr  << "Pad/Wasapi : Suspend()\n";
        m_currentState=WASS_Idle;
    }

    void Close()
    {
        if (m_currentState==WASS_Closed)
        {
            cout << "PAD/WASAPI : Close called when already closed\n";
            return;
        }
        //cerr << "Pad/Wasapi close, waiting for thread to stop...\n";
        m_currentState=WASS_Idle;
        m_threadShouldStop=true;
        if (WaitForSingleObject(m_audioThreadHandle,1000)==WAIT_TIMEOUT)
            cerr << "Pad/Wasapi close, audio thread timed out when stopping\n";
        m_currentState=WASS_Closed;
        //Sleep(500);
        CloseHandle(m_audioThreadHandle);
        m_audioThreadHandle=0;
        //if (currentDelegate)
        //    currentDelegate->StreamDidEnd(*this);
        //cout << "Pad/Wasapi close, thread was stopped\n";
    }
    void EnableMultiMediaThreadPriority(bool proAudio=false)
    {
        HMODULE hModule=LoadLibrary(L"avrt.dll");
        if (hModule)
        {
            HANDLE (WINAPI *ptrAvSetMmThreadCharacteristics)(LPCTSTR,LPDWORD);
            *((void **)&ptrAvSetMmThreadCharacteristics)=(void*)GetProcAddress(hModule,"AvSetMmThreadCharacteristicsW");
            BOOL (WINAPI *ptrAvSetMmThreadPriority)(HANDLE,AVRT_PRIORITY);
            *((void **)&ptrAvSetMmThreadPriority)=(void*)GetProcAddress(hModule,"AvSetMmThreadPriority");
            if (ptrAvSetMmThreadCharacteristics!=0 && ptrAvSetMmThreadPriority!=0)
            {
                DWORD foo=0;
                HANDLE h = 0;
                if (proAudio==false)
                    h=ptrAvSetMmThreadCharacteristics(L"Audio", &foo);
                else h=ptrAvSetMmThreadCharacteristics(L"Pro Audio", &foo);
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

    AudioCallbackDelegate* currentDelegate=nullptr;
    AudioStreamConfiguration currentConfiguration;
    vector<float> m_delegateOutputBuffer;
    vector<float> m_delegateInputBuffer;
    unsigned m_outputGlitchCounter;
    unsigned m_inputGlitchCounter;
    vector<bool> m_enabledDeviceOutputs;
    vector<bool> m_enabledDeviceInputs;
    WasapiState m_currentState;
    std::shared_ptr<std::mutex> m_mutex;
    vector<WasapiEndPoint> m_inputEndPoints;
    vector<WasapiEndPoint> m_outputEndPoints;
    volatile bool m_threadShouldStop=false;
    std::vector<int> m_supportedSampleRates;
private:
    HANDLE m_audioThreadHandle=NULL;
    string m_deviceName="Invalid";
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

    int countSupportedExclusiveFormats(const PadComSmartPointer<IAudioClient>& cl)
    {
        int result=0;
        WAVEFORMATEXTENSIBLE format; memset(&format,0,sizeof(WAVEFORMATEXTENSIBLE));
        format.SubFormat=KSDATAFORMAT_SUBTYPE_PCM;
        format.dwChannelMask=KSAUDIO_SPEAKER_STEREO;
        format.Format.wFormatTag=WAVE_FORMAT_EXTENSIBLE;
        format.Format.nChannels=2;
        format.Format.cbSize=sizeof(WAVEFORMATEXTENSIBLE);
        for (int bd : {16,24,32})
        {
            for (int sr : {44100,48000,88200,96000,176400,192000})
            {
                //cerr << "testing exclusive mode support for samplerate " << sr << " bitdepth " << bd << "\n";
                format.Format.wBitsPerSample=bd;
                format.Format.nSamplesPerSec=sr;
                format.Format.nBlockAlign=(format.Format.nChannels*format.Format.wBitsPerSample)/8;
                format.Format.nAvgBytesPerSec=format.Format.nSamplesPerSec*format.Format.nBlockAlign;
                if (cl->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,(WAVEFORMATEX*)&format,0)==S_OK)
                {
                    result++;
                    cerr << "Exclusive mode supported for " << bd << " " << sr << "\n";
                }
            }
        }

        return result;
    }
    std::pair<PadComSmartPointer<IMMDevice>,std::string> GetDefaultEndPoint(const PadComSmartPointer<IMMDeviceEnumerator>& enumerator, bool dirInput=false)
    {
        std::pair<PadComSmartPointer<IMMDevice>,std::string> result;
        PadComSmartPointer<IMMDevice> epo;
        EDataFlow direction=eRender; if (dirInput==true) direction=eCapture;
        if (CheckHResult(
            enumerator->GetDefaultAudioEndpoint(direction,eMultimedia,epo.NullAndGetPtrAddress()),
            "PAD/WASAPI : Could not get default input/output endpoint device"))
        {
            COMPointer<WCHAR> deviceId;
            epo->GetId(deviceId);
            result.second=WideCharToStdString(deviceId);
        }
        result.first=epo;
        return result;
    }

    std::string GetEndPointAdapterName(const PadComSmartPointer<IMMDevice>& epo)
    {
        PadComSmartPointer<IPropertyStore> props;
        HRESULT hr = epo->OpenPropertyStore(STGM_READ, props.NullAndGetPtrAddress());
        if (CheckHResult(hr,"PAD/WASAPI : Could not open endpoint property store")==false) return std::string();
        MyPropVariant adapterNameProp;
        hr = props->GetValue(PKEY_DeviceInterface_FriendlyName, adapterNameProp());
        if (CheckHResult(hr,"PAD/WASAPI : Could not get endpoint adapter name")==false) return std::string();
        return WideCharToStdString(adapterNameProp()->pwszVal);
    }

    std::string GetEndPointName(const PadComSmartPointer<IMMDevice>& epo)
    {
        PadComSmartPointer<IPropertyStore> props;
        HRESULT hr = epo->OpenPropertyStore(STGM_READ, props.NullAndGetPtrAddress());
        if (CheckHResult(hr,"PAD/WASAPI : Could not open endpoint property store")==false) return std::string();
        MyPropVariant endPointNameProp;
        hr = props->GetValue(PKEY_Device_FriendlyName, endPointNameProp());
        if (CheckHResult(hr,"PAD/WASAPI : Could not get endpoint name")==false) return std::string();
        return WideCharToStdString(endPointNameProp()->pwszVal);
    }

    void EnumerateEndpoints(bool enumExclusivemodeSupport)
    {
        HRESULT hr = S_OK;
        PadComSmartPointer<IMMDeviceEnumerator> enumerator;
        hr=enumerator.CoCreateInstance(CLSID_MMDeviceEnumerator,CLSCTX_ALL);
        if (CheckHResult(hr,"PAD/WASAPI : Could not create device enumerator")==false) return;
        PadComSmartPointer<IMMDeviceCollection> collection;
        PadComSmartPointer<IMMDevice> endpoint;

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
        auto defOutputInfo=GetDefaultEndPoint(enumerator,false);
        auto defInputInfo=GetDefaultEndPoint(enumerator,true);
        std::string defaultOutputDevId=defOutputInfo.second;
        std::string defaultInputDevId=defInputInfo.second;
        for (unsigned i=0;i<count;i++)
        {
            unsigned outputDeviceSortID=1; unsigned inputDeviceSortID=1;
            hr = collection->Item(i, endpoint.NullAndGetPtrAddress());
            if (CheckHResult(hr,"PAD/WASAPI : Could not get endpoint collection item")==false) continue;
            COMPointer<WCHAR> deviceId;
            endpoint->GetId(deviceId);
            std::string curDevId=WideCharToStdString(deviceId);
            if (curDevId==defaultOutputDevId)
                outputDeviceSortID=0;
            if (curDevId==defaultInputDevId)
                inputDeviceSortID=0;
            std::string adapterNameString=GetEndPointAdapterName(endpoint);
            std::string endPointNameString=GetEndPointName(endpoint);
            if (adapterNameString.size()==0 || endPointNameString.size()==0)
            {
                cerr << "PAD/WASAPI : Failed to get adapter/endpoint name strings for device " << i << "\n";
                continue;
            }
            EDataFlow audioDirection=getAudioDirection(endpoint);
            PadComSmartPointer<IAudioClient> tempClient;
            hr = endpoint->Activate(__uuidof (IAudioClient), CLSCTX_ALL,nullptr, (void**)tempClient.NullAndGetPtrAddress());
            if (tempClient==nullptr)
            {
                cerr << "pad : wasapi : could not create temp client for " << i << " to get channel counts etc\n";
                continue;
            }
            REFERENCE_TIME defPer, minPer;
            if (CheckHResult(tempClient->GetDevicePeriod(&defPer,&minPer))==false)
            {
                cerr << "PAD/WASAPI : could not get device default/min period for " << endPointNameString << "\n";
                continue;
            }
            COMPointer<WAVEFORMATEX> mixFormat;
            hr=tempClient->GetMixFormat(mixFormat);
            if (CheckHResult(hr,"PAD/WASAPI : Could not get mix format")==false) continue;
            WAVEFORMATEXTENSIBLE format;
            CopyWavFormat(format, mixFormat);
            //WAVE_FORMAT_IEEE_FLOAT
            //cout << "endpoint " << i << " format is " << mixFormat->wFormatTag << "\n";
            int defaultSr=format.Format.nSamplesPerSec;
            unsigned exclusiveModeCount=0;
            if (enumExclusivemodeSupport==true)
            {
                exclusiveModeCount=countSupportedExclusiveFormats(tempClient);
                if (exclusiveModeCount>0)
                {
                    cerr << endPointNameString << " supports exclusive mode\n";
                    adapterNameString+=" Exclusive";
                } else cerr << endPointNameString << " does not support exclusive mode\n";
            }
            wasapiMap[adapterNameString].SetName(adapterNameString);
            wasapiMap[adapterNameString].m_supportedSampleRates.push_back(defaultSr);
            //cerr << endPointNameString << " default sr is " << defaultSr << "\n";

            int minPeriodSamples=RefTimeToSamples(minPer,defaultSr);
            int defaultPeriodSamples=RefTimeToSamples(defPer,defaultSr);
            //cerr << endPointNameString << " minimum period is " << (double)minPer/10000 << " ms, default period is " << (double)defPer/10000 << " ms\n";
            //cerr << endPointNameString << " minimum buf size is " << minPeriodSamples << " , default buf size is " << defaultPeriodSamples << "\n";

            // This hasn't so far appeared to ever do anything for shared mode devices.
            // The supported samplerate has always been only the default samplerate, which we've already added
            // to the supported ones above
            for (int samplerate : {44100,48000,88200,96000,176400,192000})
            {
                if (samplerate==defaultSr)
                    continue;
                format.Format.nSamplesPerSec=(DWORD)samplerate;
                if (tempClient->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED,(WAVEFORMATEX*)&format,0)>=0)
                {
                    cerr << endPointNameString << " supports " << samplerate << " hz in shared mode\n";
                    wasapiMap[adapterNameString].m_supportedSampleRates.push_back(samplerate);
                }

            }
            if (audioDirection==eRender)
            {
                if (exclusiveModeCount==0)
                {
                    //cerr << i << " is shared output endpoint " << endPointNameString << "\n";
                    wasapiMap[adapterNameString].AddOutputEndPoint(endpoint,format.Format.nChannels,outputDeviceSortID, false, endPointNameString);
                } else
                {
                    //endPointNameString+=" [Exclusive mode]";
                    //cerr << i << " is exclusive output endpoint " << endPointNameString << "\n";
                    outputDeviceSortID=2;
                    wasapiMap[adapterNameString].AddOutputEndPoint(endpoint,format.Format.nChannels,outputDeviceSortID, true, endPointNameString);
                }
            }
            else if (audioDirection==eCapture)
            {
                if (exclusiveModeCount==0)
                {
                    //cerr << i << " is shared input endpoint " << endPointNameString << "\n";
                    wasapiMap[adapterNameString].AddInputEndPoint(endpoint,format.Format.nChannels,inputDeviceSortID, false, endPointNameString);
                } else
                {
                    //endPointNameString+=" [Exclusive mode]";
                    //cerr << i << " is exclusive input endpoint " << endPointNameString << "\n";
                    inputDeviceSortID=2;
                    wasapiMap[adapterNameString].AddInputEndPoint(endpoint,format.Format.nChannels,inputDeviceSortID, true, endPointNameString);
                }
            }
        }
    }

    void Publish(Session& PADInstance, DeviceErrorDelegate& errorHandler)
	{
        using namespace chrono;
        high_resolution_clock::time_point t1 = high_resolution_clock::now();
        CoInitialize(0);
        EnumerateEndpoints(false);
        EnumerateEndpoints(true);
        for (auto &element : wasapiMap)
        {
            element.second.InitDefaultStreamConfigurations();
            PADInstance.Register(&element.second);
        }
        high_resolution_clock::time_point t2 = high_resolution_clock::now();
        duration<double> time_span = duration_cast<duration<double>>(t2 - t1);
        std::cerr << "Enumerating WASAPI devices took " << time_span.count()*1000.0 << " ms\n";
    }
} publisher;

class WinEventContainer
{
public:
    ~WinEventContainer()
    {
        clearHandles();
    }
    void clearHandles()
    {
        int failcount=0;
        for (auto &e : m_events)
            if (!CloseHandle(e))
                failcount++;
        if (failcount>0)
            std::cerr << "WinEventContainer couldn't close all open event handles\n";
        m_events.clear();
    }
    HANDLE addEvent()
    {
        HANDLE h=CreateEvent(NULL,FALSE,FALSE,NULL);
        if (h!=NULL)
            m_events.push_back(h);
        return h;
    }
    size_t size() const { return m_events.size(); }
    HANDLE getEvent(size_t index) const
    {
        if (index>=m_events.size())
            return NULL;
        return m_events[index];
    }
    HANDLE* getEvents() { return m_events.data(); }
    bool waitForEvents(int maxwait_ms)
    {
        DWORD result=WaitForMultipleObjects(m_events.size(),m_events.data(),TRUE,maxwait_ms);
        if (result<WAIT_OBJECT_0+m_events.size())
            return true;
        return false;
    }
private:
    std::vector<HANDLE> m_events;
};

class COMInitRAIIHelper
{
public:
    COMInitRAIIHelper()
    {
        if (CoInitialize(0)>=0)
        {
            m_inited=true;
            std::cerr << "PAD/WASAPI : COM was succesfully initialized in thread " << std::this_thread::get_id() << "\n";
        }
        else std::cerr << "PAD/WASAPI : COM could not be initialized in thread " << std::this_thread::get_id() << "\n";
    }
    ~COMInitRAIIHelper()
    {
        if (m_inited==true)
        {
            std::cerr << "PAD/WASAPI : Unitializing COM in thread " << std::this_thread::get_id() << "\n";
            CoUninitialize();
        }
    }
    bool didInitialize() const { return m_inited; }
private:
    bool m_inited=false;
};

DWORD WINAPI WasapiThreadFunction(LPVOID params)
{
    WasapiDevice* dev=(WasapiDevice*)params;
    try
    {
        if (dev->m_outputEndPoints.size()==0)
        {
            return 0;
        }
        COMInitRAIIHelper com_initer;
        if (com_initer.didInitialize()==false)
            return 0;
        dev->EnableMultiMediaThreadPriority(true);
        //cout << "*** Starting wasapi audio thread "<<GetCurrentThreadId()<<"\n";
        WinEventContainer waitEvents;
        // 1 is used as a hack until rendering multiple endpoints can be properly tested
        const unsigned numOutputEndPoints=1; //dev->m_outputEndPoints.size();
        const unsigned numInputEndPoints=1;
        HRESULT hr=0;
        for (unsigned int i=0;i<numOutputEndPoints;i++)
        {
            HANDLE h=waitEvents.addEvent();
            dev->m_outputEndPoints.at(i).m_AudioClient->SetEventHandle(h);
            hr=dev->m_outputEndPoints.at(i).m_AudioClient->Start();
            CheckHResult(hr,"Wasapi Output Start audio");
        }
        for (unsigned int i=0;i<numInputEndPoints;i++)
        {
            HANDLE h=waitEvents.addEvent();
            dev->m_inputEndPoints.at(i).m_AudioClient->SetEventHandle(h);
            hr=dev->m_inputEndPoints.at(i).m_AudioClient->Start();
            CheckHResult(hr,"Wasapi Input Start audio");
        }
        int counter=0;
        int nFramesInBuffer=dev->currentConfiguration.GetBufferSize();
        BYTE* captureData=0;
        BYTE* renderData=0;
        std::vector<float> inputConvertBuffer(4096);
        //throw std::exception("test pad exceptioopn");
        while (dev->m_threadShouldStop==false)
        {
            while (dev->m_currentState==WasapiDevice::WASS_Playing)
            {
                const AudioStreamConfiguration curConf=dev->currentConfiguration;
                if (waitEvents.waitForEvents(10000)==true)
                {
                    int minFramesInput=65536;
                    for (unsigned ep=0;ep<numInputEndPoints;ep++)
                    {
                        UINT32 nFramesOfPadding;
                        hr = dev->m_inputEndPoints.at(ep).m_AudioClient->GetCurrentPadding(&nFramesOfPadding);
                        if (nFramesOfPadding==nFramesInBuffer)
                        {
                            dev->m_inputGlitchCounter++;
                        }
                        UINT32 framesInPacket=0;
                        DWORD bufferStatus=0;
                        hr = dev->m_inputEndPoints.at(ep).m_AudioCaptureClient->GetBuffer(&captureData, &framesInPacket, &bufferStatus, NULL, NULL);
                        if (hr>=0 && captureData!=nullptr)
                        {
                            if (framesInPacket<minFramesInput)
                                minFramesInput=framesInPacket;
                            float* pf=(float*)captureData;
                            unsigned numStreamChans=curConf.GetNumStreamInputs();
                            unsigned numEndpointChans=dev->m_inputEndPoints.at(ep).m_numChannels;
                            unsigned k=0;
                            float* wasapiInputBuffer=pf;
                            short* baz=(short*)captureData;
                            if (dev->m_inputEndPoints.at(ep).m_isExclusiveMode==true)
                            {
                                wasapiInputBuffer=inputConvertBuffer.data();
                                // as a hack handles just 16 bit format now

                                for (unsigned i=0;i<framesInPacket*numEndpointChans;i++)
                                {
                                    wasapiInputBuffer[i]=scale_value_from_range_to_range(baz[i],-32768,32767,-1.0,1.0);
                                    //wasapiInputBuffer[i]=-1.0+(2.0/32767)*baz[i];
                                }
                            }
                            for (unsigned i=0;i<numEndpointChans;i++)
                            {
                                if (dev->m_enabledDeviceInputs.at(i)==true)
                                {
                                    for (unsigned j=0;j<framesInPacket;j++)
                                    {
                                        dev->m_delegateInputBuffer[j*numStreamChans+k]=wasapiInputBuffer[j*numEndpointChans+i];
                                    }
                                    k++;
                                } else
                                {
                                    if (dev->m_inputEndPoints.at(ep).m_isExclusiveMode==false)
                                    {
                                        for (unsigned j=0;j<framesInPacket;j++)
                                        {
                                            dev->m_delegateInputBuffer[j*numStreamChans+k]=0.0f;
                                        }
                                    } else
                                    {
                                        for (unsigned j=0;j<framesInPacket;j++)
                                        {
                                            baz[j*numStreamChans+k]=0;
                                        }
                                    }
                                }
                            }
                            hr = dev->m_inputEndPoints.at(ep).m_AudioCaptureClient->ReleaseBuffer(framesInPacket);
                        }
                    }
                    for (unsigned ep=0;ep<numOutputEndPoints;ep++)
                    {
                        UINT32 nFramesOfPadding;
                        hr = dev->m_outputEndPoints.at(ep).m_AudioClient->GetCurrentPadding(&nFramesOfPadding);
                        if (nFramesOfPadding==nFramesInBuffer)
                        {
                            dev->m_outputGlitchCounter++;
                        }
                        unsigned framesToOutput=nFramesInBuffer-nFramesOfPadding;
                        if (minFramesInput!=65536)
                            framesToOutput=minFramesInput;
                        hr = dev->m_outputEndPoints.at(ep).m_AudioRenderClient->GetBuffer(framesToOutput, &renderData);
                        if (hr>=0 && renderData!=nullptr)
                        {
                            dev->currentDelegate->Process(0,curConf,dev->m_delegateInputBuffer.data(),dev->m_delegateOutputBuffer.data(),framesToOutput);
                            float* pf=(float*)renderData;
                            unsigned numStreamChans=curConf.GetNumStreamOutputs();
                            unsigned numEndpointChans=dev->m_outputEndPoints.at(ep).m_numChannels;
                            unsigned k=0;
                            short* wasapiOutput=(short*)renderData;
                            for (unsigned i=0;i<numEndpointChans;i++)
                            {
                                if (dev->m_enabledDeviceOutputs[i]==true)
                                {
                                    if (dev->m_outputEndPoints.at(ep).m_isExclusiveMode==false)
                                    {
                                        for (unsigned j=0;j<framesToOutput;j++)
                                        {
                                            pf[j*numEndpointChans+i]=dev->m_delegateOutputBuffer[j*numStreamChans+k];
                                        }
                                    } else
                                    {
                                        // hack, handles only 16 bit format

                                        for (unsigned j=0;j<framesToOutput;j++)
                                        {
                                            float tempsample=dev->m_delegateOutputBuffer[j*numStreamChans+k];
                                            wasapiOutput[j*numEndpointChans+i]=scale_value_from_range_to_range(tempsample,-1.0,1.0,-32768.0,32767.0);
                                            //wasapiOutput[j*numEndpointChans+i]=4095*dev->m_delegateOutputBuffer[j*numStreamChans+k];
                                        }
                                    }
                                    k++;
                                } else
                                {
                                    if (dev->m_outputEndPoints.at(ep).m_isExclusiveMode==false)
                                    {
                                        for (unsigned j=0;j<framesToOutput;j++)
                                        {
                                            pf[j*numEndpointChans+i]=0.0f;
                                        }
                                    } else
                                    {
                                        for (unsigned j=0;j<framesToOutput;j++)
                                        {
                                            wasapiOutput[j*numEndpointChans+i]=0;
                                        }
                                    }
                                }
                            }
                            hr = dev->m_outputEndPoints.at(ep).m_AudioRenderClient->ReleaseBuffer(framesToOutput, 0);
                        }
                    }
                    counter++;
                } else
                {
                    cerr << "PAD/WASAPI : Audio thread had to wait unusually long for end point events\n";
                }
            }
            //cerr << "WASAPI thread polling for playback status change...\n";
            Sleep(1);
        }
        for (unsigned i=0;i<numOutputEndPoints;i++)
        {
            hr=dev->m_outputEndPoints.at(i).m_AudioClient->Stop();
        }
        for (unsigned i=0;i<numInputEndPoints;i++)
        {
            hr=dev->m_inputEndPoints.at(i).m_AudioClient->Stop();
        }
        if (dev->m_outputGlitchCounter>0)
            cerr << "ended wasapi audio thread. "<<dev->m_outputGlitchCounter<< " glitches detected\n";
    }
    catch (std::exception& ex) { std::cerr << "PAD WASAPI audio thread exception : " << ex.what() << "\n"; }
    return 0;
}

}

