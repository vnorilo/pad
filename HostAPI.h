#pragma once

#include <vector>
#include "PAD.h"

namespace PAD{
	class HostAPIPublisher : public IHostAPI {
	friend class Session;
	protected:
		HostAPIPublisher();
		virtual void Publish(Session&, DeviceErrorDelegate&) = 0;
		virtual void Cleanup(Session&) {};
	};
}
