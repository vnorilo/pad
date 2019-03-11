#include <functional>
#include <sstream>
#include <memory>

#include <iostream> // kludge

#include "pad.h"
#include "HostAPI.h"

#include "pad_samples.h"
#include "pad_samples_sse2.h"
#include "pad_channels.h"
#include "pad_errors.h"

#pragma warning(disable: 4267)

// prevent stdint definitions by jack
#ifndef _STDINT_H
#define _STDINT_H
#endif
#include <jack/jack.h>

#ifdef WIN32
#include "utils/resourcemanagement.h"
#endif

#undef min
#undef max

#define CLEANUP(statement) Cleanup([&]() -> void { statement; return;})

namespace{
	using namespace PAD;
	using namespace std;
	class JackDevice : public AudioDevice {
		jack_client_t *client;
		string name;
		double rate;
		enum State{
			Idle,
			Initialized,
			Prepared,
			Streaming
		} currentState;
		void Unwind(State to)
		{
			if(to<currentState)
			{
				switch(currentState)
				{
				case Streaming:
					if (to >= Streaming) return;
				case Prepared:
					if (to >= Prepared) return;
					Stop();
					currentState = Prepared;
					ClearPorts();
				case Initialized:
					if (to >= Initialized) return;
					if (client) jack_client_close(client);
					client = 0;
					currentState = Idle;
				case Idle:break;
				}

			}
		}
	public:
		jack_status_t status;
		JackDevice(const string& n, double samplerate):client(0),rate(samplerate),currentState(Idle),name(n) {}
		~JackDevice() {Unwind(Idle);}
		virtual unsigned GetNumInputs() const {return 256;}
		virtual unsigned GetNumOutputs() const {return 256;}
		virtual const char *GetName() const {return "jack";}
		virtual const char *GetHostAPI() const {return "jack";}

		double CPU_Load() const { throw "Not implemented"; }

		void Init()
		{
			if (currentState < Initialized)
			{
				jack_status_t status;
				client = jack_client_open(name.c_str(),JackNullOption,&status);
				if (!client) throw PAD::HardError(PAD::DeviceDriverFailure, "Could not initialize JACK Audio Connection Kit");
				jack_set_process_callback(client,JackDevice::Process,this);
				jack_on_shutdown(client,JackDevice::Shutdown,this);
				currentState = Initialized;
			}
		}

		virtual bool Supports(const AudioStreamConfiguration& conf) const
		{
			return true;
		}

		/**
		 * Streams that the non-discerning user would most likely want
		 ***/
		virtual AudioStreamConfiguration DefaultMono() const {return AudioStreamConfiguration(rate).Input(0).Output(0);};
		virtual AudioStreamConfiguration DefaultStereo() const {return AudioStreamConfiguration(rate).StereoInput(0).StereoOutput(0);};
		virtual AudioStreamConfiguration DefaultAllChannels() const {return AudioStreamConfiguration(rate).Inputs(ChannelRange(0,8)).Outputs(ChannelRange(0,8));};


		struct JackPortList : vector<jack_port_t*>{
			~JackPortList() {assert(empty());}
			void add(jack_port_t* jp) {push_back(jp);}
			void clear(jack_client_t* c) {for(auto p : (*this)) jack_port_unregister(c,p); vector::clear();}
		};

		AudioStreamConfiguration currentConf;
		JackPortList inputPorts;
		JackPortList outputPorts;
		vector<float> clientInputBuffer, clientOutputBuffer;

		jack_nframes_t inputLatency, outputLatency;

		virtual const AudioStreamConfiguration& Open(const AudioStreamConfiguration& conf)
		{
			Unwind(Idle);
			Init();

			jack_set_process_callback(client,JackDevice::Process,this);

			if (client == 0) throw SoftError(DeviceOpenStreamFailure,"jack error");

			currentConf = conf;
			unsigned numCh = max(conf.GetNumStreamInputs(),conf.GetNumStreamOutputs());
			inputLatency = outputLatency = 0;
			for(unsigned i(0);i<numCh;++i)
			{
				char name[32];
				if (i < conf.GetNumStreamInputs())
				{
					sprintf(name,"in %i",i);
					jack_port_t* in = jack_port_register(client,name,JACK_DEFAULT_AUDIO_TYPE,JackPortIsInput,0);
					if (in) inputPorts.add(in);
					else break;

					_jack_latency_range latency;
					jack_port_get_latency_range(in, JackCaptureLatency, &latency);

					inputLatency = std::max(inputLatency, latency.max);
					
					
				}

				if (i < conf.GetNumStreamOutputs())
				{
					sprintf(name,"out %i",i);
					jack_port_t* out = jack_port_register(client,name,JACK_DEFAULT_AUDIO_TYPE,JackPortIsOutput,0);
					if (out) outputPorts.add(out);
					else break;

					_jack_latency_range latency;
					jack_port_get_latency_range(out, JackPlaybackLatency, &latency);

					outputLatency = std::max(latency.max, outputLatency);
				}
			}

			currentConf.SetDeviceChannelLimits(inputPorts.size(),outputPorts.size());
			currentConf.SetSampleRate(jack_get_sample_rate(client));
			currentConf.SetBufferSize(jack_get_buffer_size(client));
			currentState = Prepared;

			clientInputBuffer.resize(inputPorts.size() * currentConf.GetBufferSize());
			clientOutputBuffer.resize(outputPorts.size() * currentConf.GetBufferSize());

			if (currentConf.HasSuspendOnStartup() == false) Resume();
			return currentConf;
		}

		void Run() 
		{
			auto err = jack_activate(client);
			if (err) throw SoftError(DeviceStartStreamFailure,"Can't activate jack client");
		}

		void Stop()
		{
			jack_deactivate(client);
		}

		void ClearPorts()
		{
			inputPorts.clear(client);
			outputPorts.clear(client);
		}

		virtual void Resume() 
		{
			Unwind(Prepared);
			if (currentState >= Prepared)
			{
				Run();
			}
			else throw SoftError(DeviceStartStreamFailure,"jack client is not opened to stream");
		};
		
		virtual void Suspend() 
		{
			Unwind(Prepared);
		};

		virtual void Close() 
		{
			Unwind(Idle);
		};

		int Process(jack_nframes_t frames)
		{
			jack_nframes_t current_frames;
			jack_time_t current_usecs, next_usecs;
			float period_usecs;
			jack_get_cycle_times(client, &current_frames, &current_usecs, &next_usecs, &period_usecs);

			typedef Converter::HostSample<float,float,-1,1,0,SYSTEM_BIGENDIAN> jack_smp_t;
			static const unsigned channelPackage = 32;
			auto todo = inputPorts.size();
			while(todo>0)
			{
				const jack_smp_t *buffer[channelPackage];
				unsigned now = min<unsigned>(todo,channelPackage);
				for(unsigned i(0);i<now;++i) buffer[i] = (const jack_smp_t*)jack_port_get_buffer(inputPorts[i],frames);
				ChannelConverter<jack_smp_t>::Interleave(clientInputBuffer.data(),(const jack_smp_t**)buffer,frames,inputPorts.size(),inputPorts.size());
				todo-=now;
			}

			std::uint64_t inputTime = current_usecs - (inputLatency * 1000000 / currentConf.GetSampleRate());
			std::uint64_t outputTime = current_usecs + (outputLatency * 1000000 / currentConf.GetSampleRate());

			BufferSwitch(PAD::IO { 
				currentConf,
				clientInputBuffer.data(),
				clientOutputBuffer.data(),
				frames, 
				std::chrono::microseconds(inputTime),
				std::chrono::microseconds(outputTime)
			});

			todo = outputPorts.size();
			while(todo>0)
			{
				jack_smp_t *buffer[channelPackage];
				unsigned now = min<unsigned>(todo,channelPackage);
				for(unsigned i(0);i<now;++i) buffer[i] = (jack_smp_t*)jack_port_get_buffer(outputPorts[i],frames);
				ChannelConverter<jack_smp_t>::DeInterleave(clientOutputBuffer.data(),(jack_smp_t**)buffer,frames,outputPorts.size(),outputPorts.size());
				todo-=now;
			}
			return 0;
		}

		static std::chrono::microseconds GetTime() {
			return std::chrono::microseconds(jack_get_time());
		}

		virtual std::chrono::microseconds DeviceTimeNow() const {
			return GetTime();
		}

		virtual GetDeviceTime GetDeviceTimeCallback() const {
			return GetTime;
		}

		static int Process(jack_nframes_t frames, void *arg)
		{
			JackDevice *jdev = (JackDevice*)arg;
			return jdev->Process(frames);
		}

		static void Shutdown(void *obj)
		{
		}

		static void SessionCallback(void *obj)
		{
		}

	};

	struct CleanupList : public vector<function<void(void)>> {
		~CleanupList() {for(auto i(rbegin());i!=rend();++i) (*i)();}
		void operator()(function<void(void)> f) {push_back(f);}
	};


	class JackPublisher  : public HostAPIPublisher {
		vector<JackDevice> dev;
	public:
		const char *GetName() const 
		{
			return "jack";
		}

		void Publish(Session& padInstance, DeviceErrorDelegate& errorHandler)
		{
			string appName("pad_client");
#ifdef WIN32
			vector<WCHAR> buffer(64);
			GetModuleFileNameW(GetModuleHandleW(NULL),buffer.data(),buffer.size());
			appName = WideCharToStdString(buffer.data());
			size_t pos;
			while((pos = appName.find("\\"))!=appName.npos) appName=appName.substr(pos+1);
			appName = appName.substr(0,appName.rfind(".exe"));
#else
			if (getenv("_")) {
				appName = getenv("_");
				if (appName.find('/') != std::string::npos)
					appName = appName.substr(appName.find_last_of('/') + 1);
			}
#endif
			CleanupList Cleanup;
			jack_status_t status;

			try{
				jack_client_t *client = jack_client_open(appName.c_str(),JackNoStartServer,&status);
				CLEANUP(jack_client_close(client));

				if ((status&JackFailure) != 0 || client == NULL) return;

				dev.push_back(JackDevice(appName,jack_get_sample_rate(client)));
				auto &jdev(dev.back());

				padInstance.Register(&jdev);
			}
			catch(HardError s)
			{
				errorHandler.Catch(s);
			}
			catch(SoftError s)
			{
				errorHandler.Catch(s);
			}
		}

		void Cleanup()
		{
			dev.clear();
		}

	} publisher;
}

namespace PAD {
	IHostAPI* LinkJACK() {
		return &publisher;
	}
}

