#include <iostream>
#include <string>
#include <atomic>
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

#include "WinDebugStream.h"
#include "pad_samples.h"
#include "pad_samples_sse2.h"
#include "pad_channels.h"

#pragma comment(lib,"Ole32.lib")

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);

template<typename T>
inline T scale_value_from_range_to_range(double v, double inputmin, double inputmax, double outputmin, double outputmax)
{
    double range1=inputmax-inputmin;
    double range2=outputmax-outputmin;
    return T(outputmin+range2/range1*(v-inputmin));
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
    return int(x+0.5);
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
		PadComSmartPointer<IAudioClock> m_AudioClock;
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
        m_numInputs(0), m_numOutputs(0), m_currentState(WASS_Idle) 
	{
		ShowControlPanelFunc = []() { MessageBoxA(0, "This should be the WASAPI control panel", "PAD", MB_OK); };
	}
    ~WasapiDevice()
    {
        if (m_audioThreadHandle!=0)
            Close();
    }

    const char *GetName() const { return m_deviceName.c_str(); }
    const char *GetHostAPI() const { return "WASAPI"; }
    void SetName(const std::string& n) { m_deviceName=n; }
    void AddInputEndPoint(const PadComSmartPointer<IMMDevice>& ep, unsigned numChannels, unsigned sortID, bool canDoExclusive,const std::string& name=std::string())
    {
        m_numInputs+=numChannels;
        if (m_console_spam_enabled==true && sortID==0)
            cwindbg() << name << " is default input end point\n";
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
        if (m_console_spam_enabled==true && sortID==0)
            cwindbg() << name << " is default output end point\n";
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

        cwindbg() << "Supports() not implemented\n";
        return false;
    }

    virtual const AudioStreamConfiguration& Open(const AudioStreamConfiguration& conf)
    {
        //cwindbg() << "Open\n";
        if (m_currentState==WASS_Playing)
        {
            cwindbg() << "Open called when already playing\n";
            return currentConfiguration;
        }
        const unsigned endPointToActivate=0;
        //cout << "Wasapi::Open, thread id "<<GetCurrentThreadId()<<"\n";
        m_outputGlitchCounter=0;
//        currentDelegate = &conf.GetAudioDelegate();
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
                    if (theClient!=nullptr)
                    {

                        WAVEFORMATEX *pMixformat=nullptr;
                        hr = theClient->GetMixFormat(&pMixformat);
						REFERENCE_TIME hnsMinimumDuration = 0, hnsDefaultDuration;
						hr = theClient->GetDevicePeriod(&hnsDefaultDuration, &hnsMinimumDuration);
						if (m_outputEndPoints.at(endPointToActivate).m_isExclusiveMode==true)
                        {
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
                            cwindbg() << "Device default period is " << hnsDefaultDuration << "\n";
                            hr = theClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                       hnsMinimumDuration,hnsMinimumDuration,(WAVEFORMATEX*)&format, NULL);
                            if (hr<0)
                                cwindbg() << "PAD/WASAPI : Exclusive init error " << hr << "\n";
                        } else
                        {
							hr = theClient->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,hnsDefaultDuration,hnsDefaultDuration,pMixformat, NULL);
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
								if (theAudioRenderClient != nullptr)
								{
									m_outputEndPoints[endPointToActivate].m_AudioClient = theClient;
									m_outputEndPoints[endPointToActivate].m_AudioRenderClient = theAudioRenderClient;
									m_enabledDeviceOutputs.resize(m_numOutputs);
									for (unsigned i = 0; i < m_numOutputs; i++)
									{
										m_enabledDeviceOutputs[i] = currentConfiguration.IsOutputEnabled(i);
									}

									theClient->GetService(__uuidof(IAudioClock), (void**)m_inputEndPoints[endPointToActivate].m_AudioClock.NullAndGetPtrAddress());

									m_currentState = WASS_Open;
								}
								else 
									cwindbg() << "PAD/WASAPI : Audio render client was not initialized properly\n";
                            } else cwindbg() << "PAD/WASAPI : Output audio client has an unusual buffer size "<<nFramesInBuffer<<"\n";
                        }

                    } else cwindbg() << "PAD/WASAPI : Could not initialize IAudioClient\n";
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
                    if (theClient!=nullptr)
                    {
                        WAVEFORMATEX *pMixformat=nullptr;
                        hr = theClient->GetMixFormat(&pMixformat);
						REFERENCE_TIME hnsMinimumPeriod = 0, hnsDefaultPeriod = 0;
						hr = theClient->GetDevicePeriod(&hnsDefaultPeriod, &hnsMinimumPeriod);
						if (m_inputEndPoints.at(endPointToActivate).m_isExclusiveMode==true)
                        {
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
                            hr = theClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                       hnsMinimumPeriod,hnsMinimumPeriod,(WAVEFORMATEX*)&format, NULL);
                        } else
                        {
                            hr = theClient->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK,hnsDefaultPeriod,hnsDefaultPeriod,pMixformat, NULL);
                        }
                        if (CheckHResult(hr,"PAD/WASAPI : Initialize audio capture client")==true)
                        {
                            UINT32 nFramesInBuffer=0;
                            hr = theClient->GetBufferSize(&nFramesInBuffer);
							if (nFramesInBuffer > 7 && nFramesInBuffer < 32769)
							{
								m_delegateInputBuffer.resize(nFramesInBuffer*m_numInputs);
								currentConfiguration.SetBufferSize(nFramesInBuffer);
								PadComSmartPointer<IAudioCaptureClient> theAudioCaptureClient;
								hr = theClient->GetService(__uuidof(IAudioCaptureClient), (void**)theAudioCaptureClient.NullAndGetPtrAddress());
								if (theAudioCaptureClient != nullptr)
								{
									m_inputEndPoints[endPointToActivate].m_AudioClient = theClient;
									m_inputEndPoints[endPointToActivate].m_AudioCaptureClient = theAudioCaptureClient;
									m_enabledDeviceInputs.resize(m_numInputs);
									for (unsigned i = 0; i < m_numInputs; i++)
									{
										m_enabledDeviceInputs[i] = currentConfiguration.IsInputEnabled(i);
									}
									
									m_currentState = WASS_Open;
								}
								else
									cwindbg() << "PAD/WASAPI : Audio capture client was not initialized properly\n";
							} else 
								cwindbg() << "PAD/WASAPI : Input audio client has an unusual buffer size "<<nFramesInBuffer<<"\n";
                        }

                    } else cwindbg() << "PAD/WASAPI : Could not initialize IAudioClient\n";
                }
            }
        }
        if (m_currentState!=WASS_Open)
            return currentConfiguration;

		AboutToBeginStream(currentConfiguration);

        if (m_currentState==WASS_Open)
            InitAudioThread();
        if (m_currentState==WASS_Open && conf.HasSuspendOnStartup() == false)
            Run();
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
                cwindbg() << "PAD/WASAPI : Could not create audio thread :C\n";
				return;
            }
			SYSTEM_INFO sysinfo;
			GetSystemInfo(&sysinfo);
			if (sysinfo.dwNumberOfProcessors > 1)
			{
				DWORD_PTR affinityMask = 1 << 1;
				if (sysinfo.dwNumberOfProcessors > 2)
					affinityMask = 1 << 2;
				SetThreadAffinityMask(m_audioThreadHandle, affinityMask);
			}
        }
    }

    void Run()
    {
        if (m_currentState!=WASS_Open)
        {
            cwindbg() << "PAD/WASAPI : Run called when device not open\n";
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
        } else cwindbg() << "PAD/WASAPI : Resume called when audio thread not initialized\n";
    }

    void Suspend()
    {
        if (m_currentState!=WASS_Playing)
        {
            cwindbg() << "PAD/WASAPI : Suspend() called when not playing\n";
            return;
        }
        //cwindbg()  << "Pad/Wasapi : Suspend()\n";
        m_currentState=WASS_Idle;
    }

    void Close()
    {
        if (m_currentState==WASS_Closed)
        {
            cout << "PAD/WASAPI : Close called when already closed\n";
            return;
        }
        //cwindbg() << "Pad/Wasapi close, waiting for thread to stop...\n";
        m_currentState=WASS_Idle;
        m_threadShouldStop=true;
		bool did_end = false;
		if (WaitForSingleObject(m_audioThreadHandle, 1000) == WAIT_TIMEOUT)
		{
			cwindbg() << "Pad/Wasapi close : audio thread did not stop in time, trying harder in another thread...\n";
			// the thread object is leaked, but things are not so good anyway if this codepath is executed...
			std::thread* th = new std::thread([](HANDLE athandle)
			{
				if (WaitForSingleObject(athandle, 5000) == WAIT_TIMEOUT)
				{
					cwindbg() << "Pad/Wasapi close : audio thread did not stop with extra time, giving up...\n";
				}
				else
				{
					CloseHandle(athandle);
					cwindbg() << "Pad/Wasapi close : audio thread stopped and closed with extra wait time\n";
				}
			},m_audioThreadHandle);
			th->detach();
		}
		else 
			did_end = true;
		m_currentState = WASS_Closed;
		if (did_end==true)
			CloseHandle(m_audioThreadHandle);
		m_audioThreadHandle = NULL;
		StreamDidEnd();
		
    }

    double CPU_Load() const { return m_current_cpu_load; }
	
	void EnableMultiMediaThreadPriority(bool proAudio=false)
    {
        HMODULE hModule=LoadLibraryA("avrt.dll");
        if (hModule)
        {
            HANDLE (WINAPI *ptrAvSetMmThreadCharacteristics)(LPCTSTR,LPDWORD);
#ifndef UNICODE
			*((void **)&ptrAvSetMmThreadCharacteristics)=(void*)GetProcAddress(hModule,"AvSetMmThreadCharacteristicsA");
#else
			*((void **)&ptrAvSetMmThreadCharacteristics) = (void*)GetProcAddress(hModule, "AvSetMmThreadCharacteristicsW");
#endif
            BOOL (WINAPI *ptrAvSetMmThreadPriority)(HANDLE,AVRT_PRIORITY);
            *((void **)&ptrAvSetMmThreadPriority)=(void*)GetProcAddress(hModule,"AvSetMmThreadPriority");
            if (ptrAvSetMmThreadCharacteristics!=0 && ptrAvSetMmThreadPriority!=0)
            {
                DWORD foo=0;
                HANDLE h = 0;
                if (proAudio==false)
                    h=ptrAvSetMmThreadCharacteristics(TEXT("Audio"), &foo);
                else h=ptrAvSetMmThreadCharacteristics(TEXT("Pro Audio"), &foo);
                if (h != 0)
                {
                    BOOL result=ptrAvSetMmThreadPriority (h, AVRT_PRIORITY_NORMAL);
                    if (result==FALSE)
                        cwindbg() << "AvSetMmThreadPriority failed\n";
                }
                else cwindbg() << "Task wasn't returned from AvSetMmThreadCharacteristics\n";
            } else cwindbg() << "Could not resolve functions from avrt.dll\n";
        } else cwindbg() << "Could not load avrt.dll\n";
    }

//    AudioCallbackDelegate* currentDelegate=nullptr;
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
    std::atomic<bool> m_threadShouldStop=false;
    std::vector<int> m_supportedSampleRates;
    bool m_console_spam_enabled=false;
    double m_current_cpu_load=0.0;
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
                //cwindbg() << "testing exclusive mode support for samplerate " << sr << " bitdepth " << bd << "\n";
                format.Format.wBitsPerSample=bd;
                format.Format.nSamplesPerSec=sr;
                format.Format.nBlockAlign=(format.Format.nChannels*format.Format.wBitsPerSample)/8;
                format.Format.nAvgBytesPerSec=format.Format.nSamplesPerSec*format.Format.nBlockAlign;
                if (cl->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,(WAVEFORMATEX*)&format,0)==S_OK)
                {
                    result++;
                    //cwindbg() << "Exclusive mode supported for " << bd << " " << sr << "\n";
                }
            }
        }

        return result;
    }
    std::pair<PadComSmartPointer<IMMDevice>,std::string> GetDefaultEndPoint(const PadComSmartPointer<IMMDeviceEnumerator>& enumerator, bool dirInput=false)
    {
        std::pair<PadComSmartPointer<IMMDevice>,std::string> result;
        PadComSmartPointer<IMMDevice> epo;
        EDataFlow direction=eRender;
        if (dirInput==true)
            direction=eCapture;
        if (CheckHResult(
            enumerator->GetDefaultAudioEndpoint(direction,eMultimedia,epo.NullAndGetPtrAddress()),
            "PAD/WASAPI : Could not get default input/output endpoint device"))
        {
            COMPointer<WCHAR> deviceId;
            epo->GetId(deviceId.get_addr_and_free());
            result.second=WideCharToStdString(deviceId.get());
        }
        result.first=epo;
        return result;
    }

	std::chrono::microseconds DeviceTimeNow() {
		LARGE_INTEGER pc, pcFreq;
		QueryPerformanceCounter(&pc);
		QueryPerformanceFrequency(&pcFreq);

		return std::chrono::microseconds(pc.QuadPart * 1000'000ull / pcFreq.QuadPart);
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
        if (CheckHResult(hr,"PAD/WASAPI : Could not create device enumerator")==false)
            return;
        PadComSmartPointer<IMMDeviceCollection> collection;
        PadComSmartPointer<IMMDevice> endpoint;
        hr=enumerator->EnumAudioEndpoints(eAll,DEVICE_STATE_ACTIVE,collection.NullAndGetPtrAddress());
        if (CheckHResult(hr,"PAD/WASAPI : Could not enumerate audio endpoints")==false)
            return;
        UINT count=0;
        hr = collection->GetCount(&count);
        if (CheckHResult(hr,"PAD/WASAPI : Could not get endpoint collection count")==false)
            return;
        if (count == 0)
        {
            cwindbg() << "pad : wasapi : No endpoints found\n";
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
            endpoint->GetId(deviceId.get_addr_and_free());
            std::string curDevId=WideCharToStdString(deviceId.get());
            if (curDevId==defaultOutputDevId)
                outputDeviceSortID=0;
            if (curDevId==defaultInputDevId)
                inputDeviceSortID=0;
            std::string adapterNameString=GetEndPointAdapterName(endpoint);
            std::string endPointNameString=GetEndPointName(endpoint);
            if (adapterNameString.size()==0 || endPointNameString.size()==0)
            {
                cwindbg() << "PAD/WASAPI : Failed to get adapter/endpoint name strings for device " << i << "\n";
                continue;
            }
            EDataFlow audioDirection=getAudioDirection(endpoint);
            PadComSmartPointer<IAudioClient> tempClient;
            hr = endpoint->Activate(__uuidof (IAudioClient), CLSCTX_ALL,nullptr, (void**)tempClient.NullAndGetPtrAddress());
            if (tempClient==nullptr)
            {
                cwindbg() << "pad : wasapi : could not create temp client for " << i << " to get channel counts etc\n";
                continue;
            }
            REFERENCE_TIME defPer, minPer;
            if (CheckHResult(tempClient->GetDevicePeriod(&defPer,&minPer))==false)
            {
                cwindbg() << "PAD/WASAPI : could not get device default/min period for " << endPointNameString << "\n";
                continue;
            }
            COMPointer<WAVEFORMATEX> mixFormat;
            hr=tempClient->GetMixFormat(mixFormat.get_addr_and_free());
            if (CheckHResult(hr,"PAD/WASAPI : Could not get mix format")==false) continue;
            WAVEFORMATEXTENSIBLE format;
            CopyWavFormat(format, mixFormat.get());
            //WAVE_FORMAT_IEEE_FLOAT
            //cout << "endpoint " << i << " format is " << mixFormat->wFormatTag << "\n";
            int defaultSr=format.Format.nSamplesPerSec;
            unsigned exclusiveModeCount=0;
            if (enumExclusivemodeSupport==true)
            {
                exclusiveModeCount=countSupportedExclusiveFormats(tempClient);
                if (exclusiveModeCount>0)
                {
                    //cwindbg() << endPointNameString << " supports exclusive mode\n";
                    adapterNameString+=" Exclusive";
                } //else cwindbg() << endPointNameString << " does not support exclusive mode\n";
            }
            wasapiMap[adapterNameString].SetName(adapterNameString);
            wasapiMap[adapterNameString].m_supportedSampleRates.push_back(defaultSr);
            //cwindbg() << endPointNameString << " default sr is " << defaultSr << "\n";

            int minPeriodSamples=RefTimeToSamples(minPer,defaultSr);
            int defaultPeriodSamples=RefTimeToSamples(defPer,defaultSr);
            //cwindbg() << endPointNameString << " minimum period is " << (double)minPer/10000 << " ms, default period is " << (double)defPer/10000 << " ms\n";
            //cwindbg() << endPointNameString << " minimum buf size is " << minPeriodSamples << " , default buf size is " << defaultPeriodSamples << "\n";

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
                    cwindbg() << endPointNameString << " supports " << samplerate << " hz in shared mode\n";
                    wasapiMap[adapterNameString].m_supportedSampleRates.push_back(samplerate);
                }

            }
            if (audioDirection==eRender)
            {
                if (exclusiveModeCount==0)
                {
                    //cwindbg() << i << " is shared output endpoint " << endPointNameString << "\n";
                    wasapiMap[adapterNameString].AddOutputEndPoint(endpoint,format.Format.nChannels,outputDeviceSortID, false, endPointNameString);
                } else
                {
                    //endPointNameString+=" [Exclusive mode]";
                    //cwindbg() << i << " is exclusive output endpoint " << endPointNameString << "\n";
                    outputDeviceSortID=2;
                    wasapiMap[adapterNameString].AddOutputEndPoint(endpoint,format.Format.nChannels,outputDeviceSortID, true, endPointNameString);
                }
            }
            else if (audioDirection==eCapture)
            {
                if (exclusiveModeCount==0)
                {
                    //cwindbg() << i << " is shared input endpoint " << endPointNameString << "\n";
                    wasapiMap[adapterNameString].AddInputEndPoint(endpoint,format.Format.nChannels,inputDeviceSortID, false, endPointNameString);
                } else
                {
                    //endPointNameString+=" [Exclusive mode]";
                    //cwindbg() << i << " is exclusive input endpoint " << endPointNameString << "\n";
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
        //cwindbg() << "Enumerating WASAPI devices took " << time_span.count()*1000.0 << " ms\n";
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
            cwindbg() << "PAD/WASAPI : WinEventContainer couldn't close all open event handles\n";
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
        DWORD result=WaitForMultipleObjects((DWORD)m_events.size(),m_events.data(),TRUE,maxwait_ms);
        if (result<WAIT_OBJECT_0+m_events.size())
            return true;
        return false;
    }
	size_t wait(int maxwait_ms) {
		DWORD result = WaitForMultipleObjects((DWORD)m_events.size(), m_events.data(), false, maxwait_ms);
		if (result<WAIT_OBJECT_0 + m_events.size())
			return result - WAIT_OBJECT_0;
		return WAIT_FAILED;
	}

	size_t partial_wait(int first, int maxwait_ms) {
		DWORD result = WaitForMultipleObjects((DWORD)m_events.size() - first, m_events.data() + first, false, maxwait_ms);
		if (result<WAIT_OBJECT_0 + m_events.size() - first)
			return first + result - WAIT_OBJECT_0;
		return WAIT_FAILED;
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
            cwindbg() << "PAD/WASAPI : COM was succesfully initialized in thread " << std::this_thread::get_id() << "\n";
        }
        else cwindbg() << "PAD/WASAPI : COM could not be initialized in thread " << std::this_thread::get_id() << "\n";
    }
    ~COMInitRAIIHelper()
    {
        if (m_inited==true)
        {
            cwindbg() << "PAD/WASAPI : Unitializing COM in thread " << std::this_thread::get_id() << "\n";
            CoUninitialize();
        }
    }
    bool didInitialize() const { return m_inited; }
private:
    bool m_inited=false;
};

struct audio_buffer {
	std::vector<float> data;
	int position = 0;
	std::uint64_t last_device_position = 0;
	std::uint64_t position_measured_at = 0;
	bool underrun = false;

	struct operation {
		operation(const operation&) = delete;
		void operator=(operation) = delete;
		operation(operation&& from) :data(from.data), num_channels(from.num_channels), num_frames(from.num_frames), ab(from.ab) { from.num_channels = 0; }
		operation(float *data, int ch, int fr, audio_buffer& ab) :data(data), num_channels(ch), num_frames(fr), ab(ab) {}
		float *data;
		int num_channels;
		int num_frames;
		audio_buffer& ab;
	};

	struct write_buffer : public operation {
		write_buffer(write_buffer&& from) : operation(std::move(from)) {}
		write_buffer(operation&& from) :operation(std::move(from)) {}
		~write_buffer() {
			ab.position += num_frames;
		}
		inline float& operator[](int i) { assert(i >= 0 && i < num_frames*num_channels); return data[i]; }
		inline float& operator()(int frame, int ch = 0) { return operator[](frame * num_channels + ch); }
	};

	write_buffer write(int num_channels, int num_frames) {
		size_t required = (position + num_frames) * num_channels;
		if (data.size() < required) data.resize(required);
		return operation{ data.data() + position*num_channels, num_channels, num_frames, *this };
	}

	void consume(int num_channels, int num_frames) {
		assert(position * num_channels <= data.size() && num_frames <= position);
		if (num_frames == 0 || num_channels == 0) return;
		int unread = num_channels * num_frames;
		for (int i(unread);i < position * num_channels;++i) {
			data[i - unread] = data[i];
		}
		position -= num_frames;
		assert(position >= 0);
#ifndef NDEBUG
		for (int i(position*num_channels);i < data.size();++i) data[i] = 0;
#endif
	}

	struct read_buffer : public operation {
		read_buffer(read_buffer&& from) : operation(std::move(from)) {}
		read_buffer(operation&& from) : operation(std::move(from)) {}
		~read_buffer() {
			ab.consume(num_channels, num_frames);
		}
		inline float operator[](int i) const { assert(i >= 0 && i < num_frames*num_channels); return data[i]; }
		inline float operator()(int frame, int ch = 0) const { return operator[](frame * num_channels + ch); }
	};

	read_buffer read(int num_channels, int num_frames) {
		assert(num_frames <= position && position * num_channels <= data.size());
		return operation{ data.data(), num_channels, num_frames, *this };
	}
};


DWORD WINAPI WasapiThreadFunctionEvents(LPVOID params) {
	auto self = (WasapiDevice*)params;
	const auto& cfg = self->currentConfiguration;

	COMInitRAIIHelper com_init;
	assert(com_init.didInitialize());
	self->EnableMultiMediaThreadPriority(true);

	WinEventContainer wait_handles;

	const size_t num_out_endpoints = self->m_outputEndPoints.size();
	const size_t num_in_endpoints = self->m_inputEndPoints.size();

	auto& in_ep(self->m_inputEndPoints);
	auto& out_ep(self->m_outputEndPoints);

	std::vector<int> input_base, output_base;

	size_t num_outs(0);
	for (auto& ep : out_ep) {
		ep.m_AudioClient->SetEventHandle(wait_handles.addEvent());
		CheckHResult(ep.m_AudioClient->Start());
		output_base.push_back(num_outs);
		num_outs += ep.m_numChannels;
	}

	size_t num_ins(0);
	for (auto& ep : in_ep) {
		ep.m_AudioClient->SetEventHandle(wait_handles.addEvent());
		CheckHResult(ep.m_AudioClient->Start());
		input_base.push_back(num_ins);
		num_ins += ep.m_numChannels;
	}

	try {

		std::vector<int> inch_map(num_ins, -1), outch_map(num_outs, -1);
		int stream_outputs(0), stream_inputs(0);

		for (auto& cr : cfg.GetOutputRanges())
			for (auto i = cr.begin(); i != cr.end();++i)
				outch_map[i] = stream_outputs++;

		for (auto& cr : cfg.GetInputRanges())
			for (auto i = cr.begin();i != cr.end();++i)
				inch_map[i] = stream_inputs++;

		std::vector<audio_buffer> input(in_ep.size()), output(out_ep.size());
		std::vector<float> del_in, del_out;

		using exc_smp_t = HostSample<int16_t, float, -(1 << 15), (1 << 15) - 1, 0, false>;
		using smp_t = float;

		size_t stream_frames = 0;

		while (!self->m_threadShouldStop) {
			while (self->m_currentState == WasapiDevice::WASS_Playing) {
				size_t wakeup(WAIT_FAILED);
				bool has_input(true);
				for (int i(0);i < in_ep.size();++i) {
					if (!input[i].underrun && input[i].position < cfg.GetBufferSize()) {
						has_input = false;
						break;
					}
				}

				if (has_input) {
					wakeup = wait_handles.wait(100);
				} else {
					wakeup = wait_handles.partial_wait(num_out_endpoints, 100);
				}

				if (wakeup == WAIT_FAILED) continue;

				if (wakeup < num_out_endpoints) {
					// buffer swap on output side
					auto dev = wakeup;

					if (output[dev].position == 0) {
						// underflow; switch!
						auto avail = cfg.GetBufferSize() * 4;
						for (auto& ib : input) { if (ib.underrun == false && avail > ib.position) avail = ib.position; }

						size_t in_size = avail * stream_inputs;
						size_t out_size = avail * stream_outputs;
						if (del_in.size() < in_size) del_in.resize(in_size, 0);
						if (del_out.size() < out_size) del_out.resize(out_size, 0);

						IO io{
							cfg,
							(const float*)del_in.data(),
							del_out.data(),
							avail
						};

						// splat input
						for (int dev = 0; dev < in_ep.size(); ++dev) {
							const auto num_ch = in_ep[dev].m_numChannels;
							auto frames = avail;
							if (input[dev].underrun) {
								memset(del_in.data(), 0, sizeof(float)*del_in.size());
								input[dev].underrun = false;
								if (input[dev].position < frames) frames = input[dev].position;
							} else {
								io.inputBufferTime = std::chrono::microseconds(input[dev].position_measured_at / 10);
							}
							const auto src(input[dev].read(num_ch, frames));
							for (int ch = 0; ch < num_ch; ++ch) {
								int stream_ch = inch_map[input_base[dev] + ch];
								if (stream_ch >= 0) {
									for (int i = 0;i < frames;++i) del_in[i * stream_inputs + stream_ch] = src(i, ch);
								}
							}
						}

						memset(del_out.data(), 0, sizeof(float)*del_out.size());

						if (out_ep.size() && out_ep[0].m_AudioClock.getRawPointer()) {
							UINT64 streamPosBytes, pcPos, bytesPerSecond;
							out_ep[0].m_AudioClock->GetFrequency(&bytesPerSecond);
							out_ep[0].m_AudioClock->GetPosition(&streamPosBytes, &pcPos);
							std::chrono::microseconds streamPlayed(streamPosBytes * 1000'000ull / bytesPerSecond);
							std::chrono::microseconds streamRendered(stream_frames * 1000'000ull / (UINT64)cfg.GetSampleRate());
							auto latency = streamRendered - streamPlayed;
							std::chrono::microseconds systemTime(pcPos / 10);
							io.outputBufferTime = systemTime + latency;
						}

						if (self->GetBufferSwitchLock()) {
							lock_guard<recursive_mutex> lock(*self->GetBufferSwitchLock());
							self->BufferSwitch(io);
						} else {
							self->BufferSwitch(io);
						}

						stream_frames += io.numFrames;

						// splat output
						for (int dev(0);dev < out_ep.size();++dev) {
							const auto num_ch = out_ep[dev].m_numChannels;
							auto dst = output[dev].write(num_ch, avail);
														
							for (int c(0);c < num_ch;++c) {
								int stream_ch = outch_map[output_base[dev] + c];
								if (stream_ch >= 0) {
									const float *src = del_out.data() + stream_ch;
									for (int i(0);i < avail;++i) dst(i, c) = src[i * stream_outputs];
								}
							}
						}
					}

					UINT32 padding(0), avail_buffer(0);
					int avail_frames = output[dev].position;

					if (out_ep[dev].m_isExclusiveMode) {
						avail_buffer = cfg.GetBufferSize();
					} else {
						out_ep[dev].m_AudioClient->GetCurrentPadding(&padding);
						avail_buffer = cfg.GetBufferSize() - padding;
					}

					if (avail_frames > avail_buffer) {
						avail_frames = avail_buffer;
					}

					const auto num_ch = out_ep[dev].m_numChannels;

					int num_smps = avail_frames * num_ch;
					auto src = output[dev].read(num_ch, avail_frames);

					BYTE* data(nullptr);
					CheckHResult(out_ep[dev].m_AudioRenderClient->GetBuffer(avail_frames, &data), "GetBuffer");;

					if (out_ep[dev].m_isExclusiveMode) {
						exc_smp_t *dst_buffer = (exc_smp_t*)data;
						for (int i(0);i < num_smps;++i) dst_buffer[i] = src[i];
					} else {
						smp_t *dst_buffer = (smp_t*)data;
						for (int i(0);i < num_smps;++i) dst_buffer[i] = src[i];
					}

					CheckHResult(out_ep[dev].m_AudioRenderClient->ReleaseBuffer(avail_frames, 0), "ReleaseBuffer");
				} else {
					// buffer swap on input side
					BYTE* data(nullptr);
					UINT32 frames2(0), pad(0);
					UINT64 device_pos, measured_at;
					DWORD status(0);
					auto dev = wakeup - num_out_endpoints;
					const auto num_ch = in_ep[dev].m_numChannels;

					UINT32 pending(1);
					for (;pending;in_ep[dev].m_AudioCaptureClient->GetNextPacketSize(&pending)) {
						auto hr = in_ep[dev].m_AudioCaptureClient->GetBuffer(&data, &frames2, &status, &device_pos, &measured_at);
						if (hr >= 0 && data) {
							if (device_pos && (status & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
								//input[dev].underrun = true;
								cwindbg() << "x";
							}
							auto dst = input[dev].write(num_ch, frames2);
							int num_smps = num_ch * frames2;

							if (in_ep[dev].m_isExclusiveMode) {
								exc_smp_t* src_buffer = ((exc_smp_t*)data);
								for (int i = 0;i < num_smps;++i) dst[i] = src_buffer[i];
							} else {
								smp_t* src_buffer = ((smp_t*)data);
								for (int i = 0;i < num_smps;++i) dst[i] = src_buffer[i];
							}
							input[dev].last_device_position = device_pos;
							input[dev].position_measured_at = measured_at;
							in_ep[dev].m_AudioCaptureClient->ReleaseBuffer(frames2);
						}
					}
				}
			}
		}
	} catch (...) {
		for (auto ep : in_ep) ep.m_AudioClient->Stop();
		for (auto ep : out_ep) ep.m_AudioClient->Stop();
		throw;
	}
	return 0;
}

DWORD WINAPI WasapiThreadFunction(LPVOID params) {
	return WasapiThreadFunctionEvents(params);
}

}

namespace PAD {
	IHostAPI* LinkWASAPI() {
		return &publisher;
	}
}
