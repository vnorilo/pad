#include <vector>

typedef void (*AudioCallbackFunction)(uint64_t timestamp, const float* input, float *output, void *user);

class AudioStreamConfiguration {
friend class AudioDevice;
	double sampleRate;
	std::vector<std::pair<unsigned,unsigned>> inputRanges;
	std::vector<std::pair<unsigned,unsigned>> outputRanges;
	unsigned bufferSize;
	bool valid;
public:
	AudioStreamConfiguration(double sampleRate = 44100.0);
	void SetSampleRate(double);	
	void AddInputs(unsigned first = 0, unsigned last = -1);
	void AddOutputs(unsigned first = 0, unsigned last = -1);
	void SetPreferredBufferSize(unsigned frames);

	bool IsInputEnabled(unsigned index) const;
	bool IsOutputEnabled(unsigned index) const;
	bool IsValid() const {return valid;}

	unsigned GetBufferSize() const {return bufferSize;}
	double GetSampleRate() const {return sampleRate;}
};

class AudioDevice {	
public:
	virtual unsigned GetNumInputs() = 0;
	virtual unsigned GetNumOutputs() = 0;
	virtual const char *GetName() = 0;
	virtual const char *GetHostAPI() = 0;
	
	virtual bool Supports(const AudioStreamConfiguration&) = 0;

	virtual const AudioStreamConfiguration& Open(AudioCallbackFunction, bool startSuspended = false);
	
	virtual void Resume();
	virtual void Suspend();

	virtual void Close();
};