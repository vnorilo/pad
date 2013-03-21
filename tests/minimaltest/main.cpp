#include <iostream>
#include "pad.h"

using namespace std;
using namespace PAD;

class TrollersonDelegate : public PAD::AudioCallbackDelegate
{
public:
    TrollersonDelegate(const PAD::AudioStreamConfiguration& conf) : m_phase(0.0), m_conf(conf)
    {
    }
    void Process(uint64_t timestamp, const float *input, float *output,unsigned int frames)
    {
        const unsigned int numChans=m_conf.GetNumOutputs();
        const double sr=m_conf.GetSampleRate();
        for (unsigned int i=0;i<frames;i++)
        {
            float v=0.25f*sin(2*3.141592653f/sr*m_phase);
            for (unsigned int j=0;j<numChans;j++)
                output[i*numChans+j]=v;
            m_phase+=0.05;
        }
    }
private:
    double m_phase;
    PAD::AudioStreamConfiguration m_conf;
};

unsigned PAD::AudioStreamConfiguration::GetNumOutputs()
{
    return 1;
}

int main()
{
    cout << "Hello from PAD "<<PAD::versionString()<<"!"<<endl;
    //PAD::AudioStreamConfiguration conf;
    //TrollersonDelegate delegate(conf);
    for(auto& dev : PAD::AudioDevice::Enumerate())
    {
        std::cout << "[" << dev->GetHostAPI() << "] " << dev->GetName() << " : " << dev->GetNumOutputs() << "/" << dev->GetNumInputs() << "\n";
    }
    return 0;
}

