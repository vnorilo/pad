#include <string>
#include <list>
#include <regex>
#include <unordered_set>
#include <mutex>

#include "HostAPI.h"
#include "PAD.h"

namespace PAD {
	using namespace std;

	static list<HostAPIPublisher*>& AvailablePublishers( ) {
		static list<HostAPIPublisher*> __availablePublishers;
		return __availablePublishers;
	}


	class AlwaysPropagateErrors : public DeviceErrorDelegate {
	public:
		void Catch(SoftError se) { throw se; }
		void Catch(HardError he) { throw he; }
	};

	HostAPIPublisher::HostAPIPublisher( ) {
		/* todo: lock thread access */
		AvailablePublishers( ).push_back(this);
	}

	static recursive_mutex publisherMutex;
	void Session::InitializeApi(HostAPIPublisher* api, DeviceErrorDelegate &errHandler) {
		lock_guard<recursive_mutex> guard(publisherMutex);
		AvailablePublishers( ).remove(api);
		heldAPIProviders.push_back(api);
		api->Publish(*this, errHandler);
	}

	Session::Session(bool loadAll, DeviceErrorDelegate* del, void*) {
		AlwaysPropagateErrors p;
		if (del == NULL) del = &p;
		if (loadAll) {
			lock_guard<recursive_mutex> guard(publisherMutex);
			while (AvailablePublishers( ).size( )) InitializeApi(AvailablePublishers( ).front( ), *del);
		}
	}

	AudioDeviceIterator Session::FindDevice(const char *apiRegex, const char *devRegex, int minimumNumOutputs, int minimumNumInputs) {
		regex devFilter(devRegex, regex::icase);
		regex apiFilter(apiRegex, regex::icase);
		for (auto i(begin( )); i != end( ); ++i) {
			if (regex_search((*i).GetHostAPI( ), apiFilter) &&
				regex_search((*i).GetName( ), devFilter) &&
				(int)i->GetNumInputs( ) >= minimumNumInputs &&
				(int)i->GetNumOutputs( ) >= minimumNumOutputs) return i;

		}
		return end( );
	}

	AudioDeviceIterator Session::FindDevice(const char *devRegex, int minimumNumOutputs, int minimumNumInputs) {
		return FindDevice(".*", devRegex, minimumNumOutputs, minimumNumInputs);
	}

	AudioDeviceIterator Session::FindDevice(int minimumNumOutputs, int minimumNumInputs) {
		return FindDevice(".*", ".*", minimumNumOutputs, minimumNumInputs);
	}

	Session::~Session( ) {
		lock_guard<recursive_mutex> guard(publisherMutex);
		devices.clear( );
		AvailablePublishers( ).insert(AvailablePublishers( ).begin( ), heldAPIProviders.begin( ), heldAPIProviders.end( ));
		for (auto api : heldAPIProviders) api->Cleanup(*this);
	}

	void Session::InitializeHostAPI(const char *name, DeviceErrorDelegate* del) {
		string n(name);
		AlwaysPropagateErrors p;
		if (del == NULL) del = &p;

		for (auto api : AvailablePublishers( )) {
			if (n == api->GetName( )) {
				InitializeApi(api, *del);
				return;
			}
		}
		throw SoftError(UnknownApiIdentifier, "Unknown API Identifier " + n);
	}

	void Session::Register(AudioDevice* dev) {
		devices.push_back(dev);
	}

	AudioDeviceIterator Session::begin( ) { return devices.data( ); }
	AudioDeviceIterator Session::end( ) { return devices.data( ) + devices.size( ); }
};