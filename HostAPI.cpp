#include <string>
#include <list>

#include "HostAPI.h"
#include "PAD.h"

namespace PAD{
	using namespace std;

	static list<HostAPIPublisher*>& AvailablePublishers()
	{
		static list<HostAPIPublisher*> __availablePublishers;
		return __availablePublishers;
	}


	class AlwaysPropagateErrors : public DeviceErrorDelegate{
	public:
		void Catch(SoftError se) {throw se;}
		void Catch(HardError he) {throw he;}
	};

	HostAPIPublisher::HostAPIPublisher()
	{
		/* todo: lock thread access */
		AvailablePublishers().push_back(this);
	}

	void Session::InitializeApi(HostAPIPublisher* api, DeviceErrorDelegate &errHandler)
	{
		AvailablePublishers().remove(api);
		heldAPIProviders.push_back(api);
		api->Publish(*this, errHandler);
	}

	Session::Session(bool loadAll, DeviceErrorDelegate* del)
	{
		AlwaysPropagateErrors p;
		if (del == NULL) del = &p;
		if (loadAll)
		{
			while(AvailablePublishers().size()) InitializeApi(AvailablePublishers().front(),*del);
		}
	}

	Session::~Session()
	{
		AvailablePublishers().insert(AvailablePublishers().begin(),heldAPIProviders.begin(),heldAPIProviders.end());
		for(auto api : heldAPIProviders) api->Cleanup(*this);
	}

	void Session::InitializeHostAPI(const char *name, DeviceErrorDelegate* del)
	{
		string n(name);
		AlwaysPropagateErrors p;
		if (del == NULL) del = &p;

		for(auto api : AvailablePublishers())
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