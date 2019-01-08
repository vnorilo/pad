#pragma once

#include <vector>
#include "pad.h"

namespace PAD{
	class HostAPIPublisher : public IHostAPI {
	friend class Session;
	protected:
		HostAPIPublisher();
		virtual ~HostAPIPublisher() {}
		virtual void Publish(Session&, DeviceErrorDelegate&) = 0;
		virtual void Cleanup(Session&) {};
	};
}
