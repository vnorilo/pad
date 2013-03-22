#include <string>
#include <list>

#include "HostAPI.h"
#include "PAD.h"

namespace PAD{
	using namespace std;
	static list<HostAPIPublisher*> __availablePublishers;

	class AlwaysPropagateErrors : public DeviceErrorDelegate{
	public:
		void Catch(SoftError se) {throw se;}
		void Catch(HardError he) {throw he;}
	};

	HostAPIPublisher::HostAPIPublisher()
	{
		/* todo: lock thread access */
		__availablePublishers.push_back(this);
	}

	void Session::InitializeApi(HostAPIPublisher* api, DeviceErrorDelegate &errHandler)
	{
		__availablePublishers.remove(api);
		heldAPIProviders.push_back(api);
		api->Publish(*this, errHandler);
	}

	Session::Session(bool loadAll, DeviceErrorDelegate* del)
	{
		AlwaysPropagateErrors p;
		if (del == NULL) del = &p;
		if (loadAll)
		{
			while(__availablePublishers.size()) InitializeApi(__availablePublishers.front(),*del);
		}
	}

	Session::~Session()
	{
		__availablePublishers.insert(__availablePublishers.begin(),heldAPIProviders.begin(),heldAPIProviders.end());
		for(auto api : heldAPIProviders) api->Cleanup(*this);
	}

	void Session::InitializeHostAPI(const char *name, DeviceErrorDelegate* del)
	{
		string n(name);
		AlwaysPropagateErrors p;
		if (del == NULL) del = &p;

		for(auto api : __availablePublishers)
		{
			if (n == api->GetName())
			{
				InitializeApi(api,*del);
				return;
			}
		}
		throw SoftError(UnknownApiIdentifier,"Unknown API Identifier " + n);
	}

	void Session::Register(AudioDevice* dev) 
	{ 
		devices.push_back(dev);
	}

	AudioDeviceIterator Session::begin() { return devices.data(); }
	AudioDeviceIterator Session::end() { return devices.data() + devices.size(); }
};