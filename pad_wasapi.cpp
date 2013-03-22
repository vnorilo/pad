#include <iostream>
#include <string>
#include <list>

#include "winerror.h"
#include "Mmdeviceapi.h"
#include "Functiondiscoverykeys_devpkey.h"

#include "HostAPI.h"
#include "PAD.h"

#define EXIT_ON_ERROR(hres)  \
    if (FAILED(hres)) { goto Exit; }
#define SAFE_RELEASE(punk)  \
    if ((punk) != NULL)  \
{ (punk)->Release(); (punk) = NULL; }

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);

namespace
{
using namespace std;
using namespace PAD;
class WasapiDevice : public AudioDevice
{
public:
    WasapiDevice(unsigned i,const string& name, unsigned inputs, unsigned outputs):
        deviceName(name),numInputs(inputs),numOutputs(outputs),index(i) {}

    const char *GetName() const { return deviceName.c_str(); }
    const char *GetHostAPI() const { return "WASAPI"; }

    unsigned GetNumInputs() const {return numInputs;}
    unsigned GetNumOutputs() const {return numOutputs;}

    virtual bool Supports(const AudioStreamConfiguration&) const
    {
        return false;
    }

    virtual const AudioStreamConfiguration& Open(AudioCallbackDelegate&, bool startSuspended = false)
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
};

struct WasapiPublisher : public HostAPIPublisher
{
    list<WasapiDevice> devices;

	const char *GetName() const {return "WASAPI";}

    void RegisterDevice(Session& PADInstance, WasapiDevice dev)
    {
		devices.push_back(dev);
		PADInstance.Register(&devices.back());
    }

	void Publish(Session& PADInstance)
	{
        HRESULT hr = S_OK;
        IMMDeviceEnumerator *pEnumerator = NULL;
        IMMDeviceCollection *pCollection = NULL;
        IMMDevice *pEndpoint = NULL;
        IPropertyStore *pProps = NULL;
        LPWSTR pwszID = NULL;

        hr = CoCreateInstance(
                    CLSID_MMDeviceEnumerator, NULL,
                    CLSCTX_ALL, IID_IMMDeviceEnumerator,
                    (void**)&pEnumerator);
        EXIT_ON_ERROR(hr)

                hr = pEnumerator->EnumAudioEndpoints(
                    eRender, DEVICE_STATE_ACTIVE,
                    &pCollection);
        EXIT_ON_ERROR(hr)

                UINT  count;
        hr = pCollection->GetCount(&count);
        EXIT_ON_ERROR(hr)

                if (count == 0)
        {
            cerr << "No endpoints found\n";
        }
        for (unsigned i=0; i < count; i++)
        {
            hr = pCollection->Item(i, &pEndpoint);
            EXIT_ON_ERROR(hr)

                    // Get the endpoint ID string.
                    hr = pEndpoint->GetId(&pwszID);
            EXIT_ON_ERROR(hr)

                    hr = pEndpoint->OpenPropertyStore(
                        STGM_READ, &pProps);
            EXIT_ON_ERROR(hr)

            PROPVARIANT varName;
            // Initialize container for property value.
            PropVariantInit(&varName);
            hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
            EXIT_ON_ERROR(hr)
            unsigned numInputs=0; unsigned numOutputs=0;
            int sizeNeeded=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,varName.pwszVal,-1,0,0,NULL,NULL);
            if (sizeNeeded>0)
            {
                char* buf=new char[sizeNeeded];
                int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,varName.pwszVal,-1,buf,sizeNeeded,NULL,NULL);
                if (size>0)
                {
                    //cerr << "wasapi name conversion needed "<<sizeNeeded<<" bytes"<<", used "<<size<<" bytes "<<buf<<"\n";
                    RegisterDevice(PADInstance,WasapiDevice(i,string(buf),numInputs,numOutputs));
                } else
                {
                    cerr << "could not convert wasasi device name properly, using bogus name\n";
                    RegisterDevice(PADInstance,WasapiDevice(i,"Trolldevice",numInputs,numOutputs));
                }
                delete[] buf;
            } else cerr << "wasapi device name too broken, will not use this device\n";
            CoTaskMemFree(pwszID);
            pwszID = NULL;
            PropVariantClear(&varName);
            SAFE_RELEASE(pProps)
                    SAFE_RELEASE(pEndpoint)
        }
        SAFE_RELEASE(pEnumerator)
                SAFE_RELEASE(pCollection)
                return;

Exit:
        cerr << "WASAPI enumeration error\n";
        CoTaskMemFree(pwszID);
        SAFE_RELEASE(pEnumerator)
                SAFE_RELEASE(pCollection)
                SAFE_RELEASE(pEndpoint)
                SAFE_RELEASE(pProps)
    }
} publisher;
}
