#include <string>
#include <set>

#include "HostAPI.h"
#include "PAD.h"

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"

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

				drivers.loadDriver(buffer);
				ASIOGetChannels(&numInputs,&numOutputs);
				drivers.removeCurrentDriver();

				Publish(AsioDevice(i,buffer,numInputs,numOutputs));
			}
		}

		void Publish(AsioDevice& dev)
		{
			devices.push_back(dev);
			__RegisterDevice(&devices.back());
		}
	} publisher;

};