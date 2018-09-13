#pragma once
#include <Windows.h>
#include "Unknwn.h"
#include "cog/cog.h"

#include <string>
#include <vector>
#include <cstdint>
#include <algorithm>

#pragma pack(push,4)

namespace ASIO {
	struct QuadWord {
		uint32_t hi;
		uint32_t lo;
		operator uint64_t() const {
			int64_t quad = hi;
			quad <<= 32;
			quad += lo;
			return quad;
		}
	};

	typedef QuadWord Samples;
	typedef QuadWord TimeStamp;

	typedef double SampleRate;
	typedef long Bool;
	typedef long SampleType;
	typedef long Error;

	enum Truth{
		False = 0,
		True = 1
	};

	enum SampleTypeEnum{
		Int16MSB   = 0,
		Int24MSB   = 1,		// used for 20 bits as well
		Int32MSB   = 2,
		Float32MSB = 3,		// IEEE 754 32 bit floa
		Float64MSB = 4,		// IEEE 754 64 bit double float

		// these are used for 32 bit data buffer, with different alignment of the data inside
		// 32 bit PCI bus systems can be more easily used with these
		Int32MSB16 = 8,		// 32 bit data with 16 bit alignment
		Int32MSB18 = 9,		// 32 bit data with 18 bit alignment
		Int32MSB20 = 10,		// 32 bit data with 20 bit alignment
		Int32MSB24 = 11,		// 32 bit data with 24 bit alignment

		Int16LSB   = 16,
		Int24LSB   = 17,		// used for 20 bits as well
		Int32LSB   = 18,
		Float32LSB = 19,		// IEEE 754 32 bit float, as found on Intel x86 architecture
		Float64LSB = 20, 		// IEEE 754 64 bit double float, as found on Intel x86 architecture

		// these are used for 32 bit data buffer, with different alignment of the data inside
		// 32 bit PCI bus systems can more easily used with these
		Int32LSB16 = 24,		// 32 bit data with 18 bit alignment
		Int32LSB18 = 25,		// 32 bit data with 18 bit alignment
		Int32LSB20 = 26,		// 32 bit data with 20 bit alignment
		Int32LSB24 = 27,		// 32 bit data with 24 bit alignment

		//	ASIO DSD format.
		DSDInt8LSB1   = 32,		// DSD 1 bit data, 8 samples per byte. First sample in Least significant bit.
		DSDInt8MSB1   = 33,		// DSD 1 bit data, 8 samples per byte. First sample in Most significant bit.
		DSDInt8NER8	= 40		// DSD 8 bit data, 1 sample per byte. No Endianness required.
	};

	enum {
		OK = 0,             // This value will be returned whenever the call succeeded
		SUCCESS = 0x3f4847a0,	// unique success return value for ASIOFuture calls
		NotPresent = -1000, // hardware input or output is not present or available
		HWMalfunction,      // hardware is malfunctioning (can be returned by any ASIO function)
		InvalidParameter,   // input parameter invalid
		InvalidMode,        // hardware is in a bad mode or used in a bad mode
		SPNotAdvancing,     // hardware is not running when sample position is inquired
		NoClock,            // sample clock or rate cannot be determined or is not present
		NoMemory            // not enough memory for completing the request
	};

	enum TimeCodeFlags
	{
		Valid                = 1,
		Running              = 1 << 1,
		Reverse              = 1 << 2,
		Onspeed              = 1 << 3,
		Still                = 1 << 4,
		TimeCodeSpeedValid   = 1 << 8
	};

	struct TimeCode
	{       
		double          speed;                  // speed relation (fraction of nominal speed)
		// optional; set to 0. or 1. if not supported
		Samples     timeCodeSamples;			// time in samples
		unsigned long   flags;                  // some information flags (see below)
		char future[64];
	};

	struct TimeInfo
	{
		double          speed;                  // absolute speed (1. = nominal)
		TimeStamp   systemTime;             // system time related to samplePosition, in nanoseconds
												// on mac, must be derived from Microseconds() (not UpTime()!)
												// on windows, must be derived from timeGetTime()
		Samples     samplePosition;
		SampleRate  sampleRate;             // current rate
		unsigned long flags;                    // (see below)
		char reserved[12];
	};

	enum TimeInfoFlags
	{
		SystemTimeValid        = 1,            // must always be valid
		SamplePositionValid    = 1 << 1,       // must always be valid
		SampleRateValid        = 1 << 2,
		TimeInfoSpeedValid     = 1 << 3,
		SampleRateChanged      = 1 << 4,
		ClockSourceChanged     = 1 << 5
	};

	struct Time                          // both input/output
	{
		long reserved[4];                       // must be 0
		struct TimeInfo     timeInfo;       // required
		struct TimeCode     timeCode;       // optional, evaluated if (timeCode.flags & kTcValid)
	};

	struct Callbacks
	{
		void (*bufferSwitch) (long doubleBufferIndex, Bool directProcess);
		void (*sampleRateDidChange) (SampleRate sRate);
		long (*asioMessage) (long selector, long value, void* message, double* opt);
		Time* (*bufferSwitchTimeInfo) (Time* params, long doubleBufferIndex, Bool directProcess);
	};

	struct ClockSource
	{
		long index;					// as used for ASIOSetClockSource()
		long associatedChannel;		// for instance, S/PDIF or AES/EBU
		long associatedGroup;		// see channel groups (ASIOGetChannelInfo())
		Bool isCurrentSource;		// ASIOTrue if this is the current clock source
		char name[32];				// for user selection
	};

	enum AsioSelector
	{
		SelectorSupported = 1,	// selector in <value>, returns 1L if supported,
									// 0 otherwise
		EngineVersion,			// returns engine (host) asio implementation version,
									// 2 or higher
		ResetRequest,			// request driver reset. if accepted, this
									// will close the driver (ASIO_Exit() ) and
									// re-open it again (ASIO_Init() etc). some
									// drivers need to reconfigure for instance
									// when the sample rate changes, or some basic
									// changes have been made in ASIO_ControlPanel().
									// returns 1L; note the request is merely passed
									// to the application, there is no way to determine
									// if it gets accepted at this time (but it usually
									// will be).
		BufferSizeChange,		// not yet supported, will currently always return 0L.
									// for now, use ResetRequest instead.
									// once implemented, the new buffer size is expected
									// in <value>, and on success returns 1L
		ResyncRequest,			// the driver went out of sync, such that
									// the timestamp is no longer valid. this
									// is a request to re-start the engine and
									// slave devices (sequencer). returns 1 for ok,
									// 0 if not supported.
		LatenciesChanged, 		// the drivers latencies have changed. The engine
									// will refetch the latencies.
		SupportsTimeInfo,		// if host returns true here, it will expect the
									// callback bufferSwitchTimeInfo to be called instead
									// of bufferSwitch
		SupportsTimeCode,		// 
		MMCCommand,			// unused - value: number of commands, message points to mmc commands
		SupportsInputMonitor,	// SupportsXXX return 1 if host supports this
		SupportsInputGain,     // unused and undefined
		SupportsInputMeter,    // unused and undefined
		SupportsOutputGain,    // unused and undefined
		SupportsOutputMeter,   // unused and undefined
		Overload              // driver detected an overload
	};

	struct DriverInfo
	{
		long asioVersion;		// currently, 2
		long driverVersion;		// driver specific
		char name[32];
		char errorMessage[124];
		void *sysRef;			// on input: system reference
								// (Windows: application main window handle, Mac & SGI: 0)
	};

	struct ChannelInfo
	{
		long channel;			// on input, channel index
		Bool isInput;		// on input
		Bool isActive;		// on exit
		long channelGroup;		// dto
		SampleType type;	// dto
		char name[32];			// dto
	};

	struct BufferInfo
	{
		Bool isInput;			// on input:  ASIOTrue: input, else output
		long channelNum;			// on input:  channel index
		void *buffers[2];			// on output: double buffer addresses
	};

	class IASIO : public IUnknown
	{
	public:
		virtual Bool init(void *sysHandle) = 0;
		virtual void getDriverName(char *name) = 0;	
		virtual long getDriverVersion() = 0;
		virtual void getErrorMessage(char *string) = 0;	
		virtual Error start() = 0;
		virtual Error stop() = 0;
		virtual Error getChannels(long *numInputChannels, long *numOutputChannels) = 0;
		virtual Error getLatencies(long *inputLatency, long *outputLatency) = 0;
		virtual Error getBufferSize(long *minSize, long *maxSize,
			long *preferredSize, long *granularity) = 0;
		virtual Error canSampleRate(SampleRate sampleRate) = 0;
		virtual Error getSampleRate(SampleRate *sampleRate) = 0;
		virtual Error setSampleRate(SampleRate sampleRate) = 0;
		virtual Error getClockSources(ClockSource *clocks, long *numSources) = 0;
		virtual Error setClockSource(long reference) = 0;
		virtual Error getSamplePosition(Samples *sPos, TimeStamp *tStamp) = 0;
		virtual Error getChannelInfo(ChannelInfo *info) = 0;
		virtual Error createBuffers(BufferInfo *bufferInfos, long numChannels,
			long bufferSize, Callbacks *callbacks) = 0;
		virtual Error disposeBuffers() = 0;
		virtual Error controlPanel() = 0;
		virtual Error future(long selector,void *opt) = 0;
		virtual Error outputReady() = 0;
	};

	struct DriverRecord {
		int driverID;
		CLSID classID;
		std::string driverName;
		COG::ComRef<IASIO> driverObject;
		std::shared_ptr<COG::Holder> com;
	};

	std::vector<DriverRecord> GetDrivers();
}

#pragma pack(pop)
