#include <iostream>
#include "PAD.h"

int main()
{
	using namespace PAD;
	Session myAudioSession;

	for(auto& dev : myAudioSession)
	{
		std::cout << "[" << dev.GetHostAPI() << "] " << dev.GetName() << " : " << dev.GetNumOutputs() << "/" << dev.GetNumInputs() << "\n";
	}
}