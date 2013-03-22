#pragma once

#include <vector>

namespace PAD{
	class AudioDevice;
	class Session;

	class HostAPIPublisher {
	friend class Session;
	protected:
		HostAPIPublisher();
		virtual void Publish(Session&) = 0;
		virtual void Cleanup(Session&) {};
	public:
		virtual const char *GetName() const = 0;
	};
}
