#ifndef PAD_EXPORT_STATIC_LIB
#include <iostream>
#include "pad.h"
#define _USE_MATH_DEFINES
#include <math.h>
#include <thread>

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
    ErrorLogger el;
    Session myAudioSession(true,&el);
    
    for(auto&& d : myAudioSession) {
        std::cout << "Testing " << d.GetName() << "\n" << d.DefaultAllChannels() << "\n";
        double phase = 0.0;
        try {
            float maxInput = 0;
            if (d.DefaultAllChannels().GetNumStreamOutputs() > 0) {
                d.BufferSwitch = [&](PAD::IO io) {
                    int nc = io.config.GetNumStreamOutputs();
                    int ni = io.config.GetNumStreamInputs();
                    for(int c =0;c<nc;++c) {
                        for(int i=0;i<io.numFrames;++i) {
                            io.output[i*nc+c] = sin(phase + 0.02 * M_PI * i);
                        }
                    }
                    phase += 0.02 * M_PI * io.numFrames;
                    for(int c=0;c<ni*io.numFrames;++c) {
                        maxInput = std::max(maxInput, io.input[c]);
                    }
                };
            }
            auto defaultConf = d.DefaultAllChannels();
            d.Open(defaultConf);
            std::this_thread::sleep_for(std::chrono::seconds(10));
            d.Close();
            if (defaultConf.GetNumStreamInputs()) {
                std::cout << "Maximum input level: " << maxInput << std::endl;
            }
        } catch(std::exception& e) {
            std::cout << "* ERROR " << e.what() << " *\n";
        }
    }
    
    return 0;
}

#endif
