#ifndef PAD_EXPORT_STATIC_LIB
#include <iostream>
#include "pad.h"
#define _USE_MATH_DEFINES
#include <math.h>
#include "Windows.h"

using namespace std;
using namespace PAD;

class ErrorLogger : public DeviceErrorDelegate {
public:
    void Catch(SoftError e) {std::cerr << "*Soft "<<e.GetCode()<<"* :" << e.what() << "\n";}
    void Catch(HardError e) {std::cerr << "*Hard "<<e.GetCode()<<"* :" << e.what() << "\n";}
};

int main()
{
    cout << "Hello from PAD "<<PAD::VersionString()<<"!"<<endl;
    //getchar();
    Session myAudioSession(true,&ErrorLogger());
    //myAudioSession.InitializeHostAPI("ASIO",&ErrorLogger());

    for(auto& dev : myAudioSession)
    {
        std::cout << dev << "\n  * Stereo : " << dev.DefaultStereo()
                  << "\n  * All    : " << dev.DefaultAllChannels() << "\n\n";
    }
    //return 0;
    //auto audioDevice = myAudioSession.FindDevice("2- High Definition Audio Device");
    // 2- Scarlett 2i2 USB [2x2]
    //auto audioDevice = myAudioSession.FindDevice("Focusrite USB 2.0 Audio Driver");
    auto audioDevice = myAudioSession.FindDevice("2- Scarlett 2i2 USB");
    //getchar();
    if (audioDevice != myAudioSession.end())
    {
        double phase = 0;
        int lastCBSize=-1;
        auto myAudioProcess = Closure(([&](uint64_t time, const PAD::AudioStreamConfiguration& cfg, const float *input, float *output, unsigned frames)
        {
                                          if (lastCBSize!=frames)
                                          {
                                              cerr << frames << "\n";
                                              lastCBSize=frames;
                                          }

                                          unsigned numOuts(cfg.GetNumStreamOutputs());
                                          unsigned numIns(cfg.GetNumStreamInputs());
                                          for(unsigned i(0);i<frames;++i)
                                          {
                                              float insample=input[i*numIns+0];
                                              for(unsigned j(0);j<numOuts;++j)
                                              {


                                                  if (input && output)
                                                  {

                                                      float gainfactor=0.5+0.5*( (float)sin(phase * (1.0 + double(j)/numOuts)));
                                                      output[i*numOuts+j] = insample; //*gainfactor;
                                                  }

                                                  //if (j<numIns) output[i*numOuts+j] = input[i*numIns+j];
                                                  phase = phase + 0.01 * M_PI;
                                              }
                                          }
                                      }));

        AudioStreamConfiguration conf;
        //conf.SetSampleRate(22050.0);
        conf.SetAudioDelegate(myAudioProcess);
        conf.AddDeviceOutputs(ChannelRange(0,2));
        conf.AddDeviceInputs(ChannelRange(0,2));
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

        /*
        std::cout << "Actual stream parameters: " <<
                             audioDevice->Open(Stream(myAudioProcess).SampleRate(44100)
                                              .StereoInput(0).StereoOutput(0)
                                             .Delegate(myAudioProcess)) << "\n";*/
        //getchar();
    }
    //getchar();
    return 0;
}

#endif
