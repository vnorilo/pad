#include <iostream>
#include "PAD.h"

int main()
{
	PAD::Session myAudioSession;

	for(auto& dev : myAudioSession)
	{
		std::cout << dev << "\n  * Stereo : " << dev.DefaultStereo() 
						 << "\n  * All    : " << dev.DefaultAllChannels() << "\n\n";
	}
}