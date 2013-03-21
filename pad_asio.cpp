#include <string>

#include "HostAPI.h"
#include "PAD.h"

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"
#include <iostream>

namespace {
	using namespace std;
	using namespace PAD;
	
	AsioDrivers drivers;

	class AsioDevice : public AudioDevice {
		string deviceName;
		unsigned numInputs, numOutputs;

		unsigned minSize;
		unsigned maxSize;
		unsigned preferredSize;
		unsigned index;
	public:
		AsioDevice(unsigned i,const string& name, unsigned inputs, unsigned outputs):
			deviceName(name),numInputs(inputs),numOutputs(outputs),index(i) {}

		const char *GetName() const { return deviceName.c_str(); }
		const char *GetHostAPI() const { return "ASIO"; }

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

		static AudioDeviceCollection Enumerate();
	};

	struct AsioPublisher{		
		vector<AsioDevice> devices;
		AsioPublisher()
		{
			const unsigned MaxNameLength = 64;
			unsigned numDrivers = drivers.asioGetNumDev();
            for(unsigned i(0);i<numDrivers;++i)
			{
				char buffer[MaxNameLength];
				drivers.asioGetDriverName(i,buffer,MaxNameLength);
                long numInputs = 0;
				long numOutputs = 0;

                if (drivers.loadDriver(buffer)==true)
                {
                    ASIOError err=ASIOGetChannels(&numInputs,&numOutputs);
                    if (err!=ASE_OK)
                        cout << "getting channel counts didn't work" << i << err << std::endl;
                    drivers.removeCurrentDriver();

                    devices.push_back(AsioDevice(i,buffer,numInputs,numOutputs));
                    Publish(devices.back());
                } else cout << "loading driver didn't work" << std::endl;
			}
		}

		void Publish(AsioDevice& dev)
		{
			__RegisterDevice(&dev);
		}
	} publisher;

}
