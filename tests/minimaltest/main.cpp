#include <iostream>
#include "pad.h"
#define _USE_MATH_DEFINES
#include <math.h>
using namespace std;
using namespace PAD;
/*
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
*/
int main()
{
    cout << "Hello from PAD "<<PAD::VersionString()<<"!"<<endl;
    using namespace PAD;
    Session myAudioSession;
    for(auto& dev : myAudioSession)
    {
        std::cout << dev << "\n  * Stereo : " << dev.DefaultStereo()
                  << "\n  * All    : " << dev.DefaultAllChannels() << "\n\n";
    }
    return 0;
    /*
    auto rme(myAudioSession("ASIO Fireface"));
    if (rme)
    {
        double phase = 0;
        auto audioProcess = Closure(([&](uint64_t time, const PAD::AudioStreamConfiguration&, const float *input, float *output, unsigned frames)
        {
                                        for(unsigned i(0);i<frames;++i)
                                        {
                                            output[i] = sin(phase);
                                            phase = phase + 0.01 * M_PI;
                                        }
                                    }));

        rme->Open(SampleRate(44100) + Outputs(0,4),audioProcess);
        getchar();
        rme->Close();
    }
    */
    return 0;
}

