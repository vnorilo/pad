#ifndef PAD_EXPORT_STATIC_LIB
#include <iostream>
#include "PAD.h"
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
      if (d.DefaultAllChannels().GetNumStreamOutputs() > 0) {
        d.BufferSwitch = [&](PAD::IO io) { 
          int nc = io.config.GetNumStreamOutputs();
          for(int i=0;i<io.numFrames;++i) {
            for(int c =0;c<nc;++c) {
              io.output[i*nc+c] = sin(phase);
            }
            phase += 0.02 * M_PI;
          }
        };
      }
	  auto defaultConf = d.DefaultAllChannels();
	  defaultConf.SetBufferSize(32);
	  defaultConf.SetOutputRanges({ 0 });
	  d.Open(defaultConf);
      std::this_thread::sleep_for(std::chrono::seconds(10));
      d.Close();
      } catch(std::exception& e) {
        std::cout << "* ERROR " << e.what() << " *\n";
      }
    }

    return 0;
}

#endif
