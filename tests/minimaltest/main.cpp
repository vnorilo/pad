#include <iostream>
#include "pad.h"
#define _USE_MATH_DEFINES
#include <math.h>
#include "Windows.h"

using namespace std;
using namespace PAD;

int main()
{
    cout << "Hello from PAD "<<PAD::VersionString()<<"!"<<endl;
    //getchar();
    using namespace PAD;
    class ErrorLogger : public DeviceErrorDelegate {
    public:
        void Catch(SoftError e) {std::cerr << "*Soft "<<e.GetCode()<<"* :" << e.what() << "\n";}
        void Catch(HardError e) {std::cerr << "*Hard "<<e.GetCode()<<"* :" << e.what() << "\n";}
    };

    Session myAudioSession(true,&ErrorLogger());


    for(auto& dev : myAudioSession)
    {
        std::cout << dev << "\n  * Stereo : " << dev.DefaultStereo()
                  << "\n  * All    : " << dev.DefaultAllChannels() << "\n\n";
    }
    //return 0;
    auto audioDevice = myAudioSession.FindDevice("E-MU ASIO");
    //getchar();
    if (audioDevice != myAudioSession.end())
    {
        double phase = 0;
        auto myAudioProcess = Closure(([&](uint64_t time, const PAD::AudioStreamConfiguration& cfg, const float *input, float *output, unsigned frames)
        {
            unsigned numOuts(cfg.GetNumStreamOutputs());
            unsigned numIns(cfg.GetNumStreamInputs());
            for(unsigned i(0);i<frames;++i)
            {
                for(unsigned j(0);j<numOuts;++j)
                {
                    if (output)
                        output[i*numOuts+j] = 0.1*( (float)sin(phase * (1.0 + double(j)/numOuts)));
                    //if (j<numIns) output[i*numOuts+j] = input[i*numIns+j];
                    phase = phase + 0.01 * M_PI;
                }
            }
        }));

        AudioStreamConfiguration conf;
        //conf.SetSampleRate(22050.0);
        conf.SetAudioDelegate(myAudioProcess);
        conf.AddDeviceOutputs(ChannelRange(0,2));
        //for (int i=0;i<64;i++)
        //{
            //cout << "lifecycle pass " << i << ", opening device and playing audio...\n";
            AudioStreamConfiguration actualConf=audioDevice->Open(conf);
            std::cout << "actual stream parameters " << actualConf << "\n";
            std::cout << "actual buffer size is "<<actualConf.GetBufferSize()<<"\n";
            std::cout << "device name is " << audioDevice->GetName() << "\n";
            getchar();
            //Sleep(100);
            audioDevice->Close();
            //Sleep(2000);
            //cout << "device closed for pass " << i << "\n";
            //cout << "\n";
        //}
    }
    //getchar();
    return 0;
}

