#include <iostream>
#include "PAD.h"

int main()
{
	using namespace PAD;

	for(auto& dev : AudioDevice::Enumerate())
	{
		std::cout << "[" << dev.GetHostAPI() << "]" << dev.GetName() << " : " << dev.GetNumOutputs() << "/" << dev.GetNumInputs() << "\n";
	}
}