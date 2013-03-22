#pragma once

#include <vector>

namespace PAD{
	class AudioDevice;
	class Session;
	class DeviceErrorDelegate;

	class HostAPIPublisher {
	friend class Session;
	protected:
		HostAPIPublisher();
		virtual void Publish(Session&, DeviceErrorDelegate&) = 0;
		virtual void Cleanup(Session&) {};
	public:
		virtual const char *GetName() const = 0;
	};
}
