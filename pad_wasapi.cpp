#include <string>
#include "HostAPI.h"
#include "PAD.h"
#include <iostream>

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
}
