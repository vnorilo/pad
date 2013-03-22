#include <iostream>
#include "PAD.h"

int main()
{
	using namespace PAD;

	for(auto& dev : AudioDevice::Enumerate())
	{
		std::cout << "[" << dev.GetHostAPI() << "] " << dev.GetName() << " : " << dev.GetNumOutputs() << "/" << dev.GetNumInputs() << "\n";

		AudioStreamConfiguration conf(44100);

		ChannelRange inputs[] = {ChannelRange(0,2), ChannelRange(3,4)};
		conf.AddInputs(inputs);
	}
}