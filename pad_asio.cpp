#include <string>
#include <set>
#include <list>

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

		set<double> knownGoodSampleRates;

		bool SetAsCurrentDriver(bool stopActiveStream)
		{
		}

		bool SupportsSampleRate(double rate)
		{
			if (knownGoodSampleRates.find(rate) != knownGoodSampleRates.end()) return true;			
			if (SetAsCurrentDriver(false))
			{
				if (ASIOCanSampleRate(rate) == ASE_OK)
				{
					knownGoodSampleRates.insert(rate);
					return true;
				}
				else return false;
			}
			else return false;
		}

	public:
		AsioDevice(unsigned i,const string& name, unsigned inputs, unsigned outputs):
			deviceName(name),numInputs(inputs),numOutputs(outputs),index(i) {}

		const char *GetName() const { return deviceName.c_str(); }
		const char *GetHostAPI() const { return "ASIO"; }

		unsigned GetNumInputs() const {return numInputs;}
		unsigned GetNumOutputs() const {return numOutputs;}

		virtual bool Supports(const AudioStreamConfiguration& conf) const
		{
			if (conf.GetNumInputs() > GetNumInputs() ||
				conf.GetNumOutputs() > GetNumOutputs()) return false;
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
	};

	struct AsioPublisher : public HostAPIPublisher {		
		list<AsioDevice> devices;
		void RegisterDevice(Session& PADInstance, AsioDevice dev)
		{
			devices.push_back(dev);
			PADInstance.Register(&devices.back());
		}

		const char* GetName() const {return "ASIO";}

		void Publish(Session& PADInstance)
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
                        cerr << "getting channel counts didn't work." << i << err << "\n";
                    drivers.removeCurrentDriver();

					RegisterDevice(PADInstance,AsioDevice(i,buffer,numInputs,numOutputs));
                } else cerr << "loading driver didn't work\n";
			}
		}
	} publisher;

}
