#pragma comment(lib,"Ole32.lib")
#pragma comment(lib,"Winmm.lib")
#pragma comment(lib,"RTWorkQ.lib")

#define NO_MINMAX
#include "pad.h"
#include "HostAPI.h"

#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <Avrt.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <RTWorkQ.h>

#include "cog/cog.h"

#include <codecvt>
#include <string.h>
#include <unordered_map>
#include <unordered_set>
#include <iostream> // temp
#include <algorithm>
#include <atomic>
#include <array>

#include "WinDebugStream.h"


namespace {
	using namespace COG;

	namespace ID {
		const CLSID DeviceEnumerator = __uuidof(::MMDeviceEnumerator);
	}

	using namespace PAD;
	struct WasapiDevice : public AudioDevice {

		using AudioClientRef = ComRef<IAudioClient>;

		enum State {
			Stopped,
			Suspended,
			Streaming
		} state;

		struct Configuration {
			int defaultSampleRate = 0;
			std::vector<std::pair<int,int>> inputChannel;
			std::vector<std::pair<int,int>> outputChannel;
		};

		mutable std::unique_ptr<Configuration> config;

		const Configuration& Cfg() const {
			if (!config) {
				config = std::make_unique<Configuration>();
				
				WinError::Context("Retrieving device capabilities");

				auto channelMapper = [&](auto& ports, auto& endpointChannels) {
					for (int i = 0;i < ports.size();++i) {
						WAVEFORMATEX* format;
						WinError err = ports[i]->GetMixFormat(&format);
						config->defaultSampleRate = format->nSamplesPerSec;
						for (int c = 0; c < format->nChannels; ++c) {
							endpointChannels.emplace_back(i, c);
						}
					}
				};

				channelMapper(inputPorts, config->inputChannel);
				channelMapper(outputPorts, config->outputChannel);
			}
			return *config;
		}

		static WAVEFORMATEXTENSIBLE Canonical(int numChannels, int sampleRate) {
			WAVEFORMATEX fmt;
			fmt.cbSize = sizeof(WAVEFORMATEXTENSIBLE);
			fmt.nChannels = numChannels;
			fmt.nSamplesPerSec = sampleRate;
			fmt.wBitsPerSample = 32;
			fmt.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
			fmt.nBlockAlign = 4 * numChannels;
			fmt.nAvgBytesPerSec = sampleRate * 4 * numChannels;
			WAVEFORMATEXTENSIBLE fmtex;
			fmtex.Format = fmt;
			fmtex.Samples.wValidBitsPerSample = 32;
			fmtex.dwChannelMask = KSAUDIO_SPEAKER_DIRECTOUT; // (1 << numChannels) - 1;
			fmtex.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
			return fmtex;
		}

		std::vector<AudioClientRef> inputPorts, outputPorts;
		std::string name;

		WasapiDevice(std::string name) :name(name) { }

		void Configure() {
			Cfg();
		}

		unsigned int GetNumInputs() const {
			return (unsigned)Cfg().inputChannel.size();
		}

		unsigned int GetNumOutputs() const {
			return (unsigned)Cfg().outputChannel.size();
		}

		const char *GetName() const {
			return name.c_str();
		}

		const char *GetHostAPI() const {
			return "WASAPI";
		}

		bool Supports(const AudioStreamConfiguration& cfg) const {
			WinError::Context("Querying format support");
			std::unordered_set<IAudioClient*> endpoints;
			for (auto range : cfg.GetInputRanges()) {
				for (auto c = range.begin(); c != range.end(); ++c) {
					endpoints.emplace(inputPorts[Cfg().inputChannel[c].first].Get());					
				}
			}

			for (auto range : cfg.GetOutputRanges()) {
				for (auto c = range.begin(); c != range.end(); ++c) {
					endpoints.emplace(outputPorts[Cfg().outputChannel[c].first].Get());
				}
			}

			for (auto ep : endpoints) {
				WAVEFORMATEX *mixFormat;
				WinError err = ep->GetMixFormat(&mixFormat);
				auto testFormat = Canonical(mixFormat->nChannels, (int)cfg.GetSampleRate());
				switch (ep->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, (WAVEFORMATEX*)&testFormat, nullptr)) {
					case S_OK: continue;
					default: return false;
				}
				
			}
			return true;
		}

		AudioStreamConfiguration Default(unsigned numInputs, unsigned numOutputs) const {
			AudioStreamConfiguration def(Cfg().defaultSampleRate, true);
			if (Cfg().outputChannel.size() < numOutputs) {
				return AudioStreamConfiguration(0.0, false);
			}
			def.AddDeviceOutputs(ChannelRange(0, numOutputs));
			def.AddDeviceInputs(ChannelRange(0, std::min<unsigned>(numInputs, (unsigned)Cfg().inputChannel.size())));
			REFERENCE_TIME bufferSz, min;
			outputPorts.front()->GetDevicePeriod(&bufferSz, &min);
			bufferSz *= (REFERENCE_TIME)def.GetSampleRate();
			def.SetBufferSize((unsigned)bufferSz / 10'000'000);
			return def;
		}

		AudioStreamConfiguration DefaultMono() const {
			return Default(1, 1);
		}

		AudioStreamConfiguration DefaultStereo() const {
			return Default(2, 2);
		}

		AudioStreamConfiguration DefaultAllChannels() const {
			return Default((unsigned)Cfg().inputChannel.size(), (unsigned)Cfg().outputChannel.size());
		}

		struct IStream { virtual ~IStream() {} virtual void Activate(bool) = 0; virtual const AudioStreamConfiguration& Config() const = 0; };

		std::unique_ptr<IStream> stream;

		template <int SIZE>
		class RingBuffer {
			std::array<float, SIZE> buffer;
			unsigned read = 0;
			unsigned write = 0;
			unsigned Available(unsigned p, unsigned limit) const {
				unsigned avail = (limit - p) & mask;
				return avail;
			}

			float *Op(unsigned maxFrames, unsigned &actualFrames, unsigned p, unsigned limit) {
				auto avail = Available(p, limit);
				const unsigned contg = SIZE - p;
				actualFrames = std::min(maxFrames, std::min(contg, avail));
				return buffer.data();
			}
		public:

			struct Region {
				~Region() {
					commit += size;
				}
				unsigned& commit;
				unsigned size;
				float *const region;
				float& operator[](unsigned i) {
					assert(i<size);
					return region[i];
				}
			};

			static constexpr int mask = (SIZE - 1);
			static_assert((SIZE & (SIZE - 1)) == 0, "RingBuffer size must be a power of two");


			Region Write(unsigned maxFrames) {
				unsigned actualFrames;
				float* ptr = Op(maxFrames, actualFrames, write, read);
				if (actualFrames == 0) actualFrames = std::min<unsigned>(SIZE, maxFrames);
				return Region{ write, actualFrames, ptr };
			}

			unsigned Write(const float* source, unsigned frames) {
				unsigned written = 0;
				while (frames) {
					auto wr = Write(frames);
					if (wr.size == 0) return written;
					memcpy(wr.region, source, sizeof(float) * wr.size);
					source += wr.size;
					frames -= wr.size;
					written += wr.size;
				}
				return written;
			}

			unsigned WriteAvailable() const {
				return Available(write, read);
			}

			Region Read(unsigned maxFrames) {
				unsigned actualFrames;
				float* ptr = Op(maxFrames, actualFrames, read, write);
				return Region{ read, actualFrames, ptr };
			}

			unsigned ReadAvailable() const {
				return Available(read, write);
			}

		};

		template <typename T> struct PortMap {
			using ServiceTy = T;
			ComRef<T> service;
			std::vector<std::pair<int, int>> map;
			RingBuffer<4096> data;
			unsigned numEpChannels;
			size_t GetNumChannels() const {
				return numEpChannels;
			}
		};

		template <typename PORTMAP>
		void InitializePort(AudioStreamConfiguration& cfg, IAudioClient* ac, PORTMAP& pm, HANDLE bufferSwitch) {
			WinError err;
			IAudioClient3* ac3 = nullptr;

			WAVEFORMATEX* fmt;
			err = ac->GetMixFormat(&fmt);
			WAVEFORMATEXTENSIBLE *fmtEx = (WAVEFORMATEXTENSIBLE*)fmt;
			auto mixFormat = Canonical(fmt->nChannels, (int)cfg.GetSampleRate());
			pm.numEpChannels = fmt->nChannels;

			if (S_OK == ac->QueryInterface(&ac3)) {
				// win10
				UINT32 defaultPeriod(0), fundamentalPeriod(0), minPeriod(0), maxPeriod(0);
				err = ac3->GetSharedModeEnginePeriod((WAVEFORMATEX*)&mixFormat,
													 &defaultPeriod, &fundamentalPeriod, &minPeriod, &maxPeriod);
				if (cfg.GetBufferSize() <= minPeriod) {
					cfg.SetBufferSize(minPeriod);
				} else if (cfg.GetBufferSize() < defaultPeriod) {
					cfg.SetBufferSize(defaultPeriod);
				} else {
					cfg.SetBufferSize(maxPeriod);
				}

				err = ac3->InitializeSharedAudioStream(
					AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
					cfg.GetBufferSize(),
					(WAVEFORMATEX*)&mixFormat,
					nullptr);

				if (AUDCLNT_E_ENGINE_PERIODICITY_LOCKED == err.Ignore({ S_OK, AUDCLNT_E_ENGINE_PERIODICITY_LOCKED })) {
					UINT32 current;
					err = ac3->GetCurrentSharedModeEnginePeriod(&fmt, &current);
					cfg.SetBufferSize(current);
				}
			} else {
				// pre-win10
				cfg.SetSampleRate(fmt->nSamplesPerSec);
				REFERENCE_TIME defaultPeriod, lowLatencyPeriod;
				err = ac->GetDevicePeriod(&defaultPeriod, &lowLatencyPeriod);
				lowLatencyPeriod = lowLatencyPeriod * fmt->nSamplesPerSec / 10'000'000;
				defaultPeriod = defaultPeriod * fmt->nSamplesPerSec / 10'000'000;
				if (cfg.GetBufferSize() <= lowLatencyPeriod) {
					cfg.SetBufferSize((unsigned)lowLatencyPeriod);
				} else {
					cfg.SetBufferSize((unsigned)defaultPeriod);
				}
				REFERENCE_TIME hnsBuffer = cfg.GetBufferSize() * 10'000'000 / (unsigned)cfg.GetSampleRate();
				err = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsBuffer, hnsBuffer, (WAVEFORMATEX*)&mixFormat, nullptr);
			}

			if (bufferSwitch != INVALID_HANDLE_VALUE) ac->SetEventHandle(bufferSwitch);
			err = ac->GetService(__uuidof(typename PORTMAP::ServiceTy), (void**)pm.service.Reset());
		}

		const AudioStreamConfiguration& Open(const AudioStreamConfiguration& cfg) {
			stream.reset();
			struct Stream : public IStream, public IRtwqAsyncCallback {
				AudioStreamConfiguration cfg;
				WasapiDevice* dev;
				std::atomic_flag streaming;
				std::atomic_flag runTask;

				size_t rendered = 0;
				ComRef<IAudioClock> clock;

				std::vector<float> delegateIn, delegateOut;

				struct RTWQState {
					DWORD Queue, Task;
					HANDLE Event, SecondaryEvent;
					HANDLE Done;
					ComRef<IRtwqAsyncResult> Result;
				} RTWQ;

				bool InputUnityMapped, OutputUnityMapped;
				std::unordered_map<IAudioClient*, PortMap<IAudioCaptureClient>> in;
				std::unordered_map<IAudioClient*, PortMap<IAudioRenderClient>> out;

				STDMETHOD_(ULONG, AddRef)() throw() { return 0; }
				STDMETHOD_(ULONG, Release)() throw() { return 0; }
				STDMETHOD(QueryInterface)(REFIID, _COM_Outptr_ void** ppvObject) throw() {
					*ppvObject = NULL;
					return E_NOINTERFACE;
				}

				STDMETHODIMP GetParameters(DWORD* flags, DWORD *queue) override {
					*flags = 0; *queue = RTWQ.Queue; return S_OK;
					return S_OK;
				}

				void SplatInput(PAD::IO& io) {
					auto streamIns = cfg.GetNumStreamInputs();
					auto frames = io.numFrames;
					unsigned gap = 0;

					if (delegateIn.size() < streamIns * frames) {
						delegateIn.resize(streamIns * frames);
					}

					for (auto &ep : in) {
						auto numCh = ep.second.GetNumChannels();
						auto toDo = ep.second.data.ReadAvailable() / numCh;

						if (toDo > io.numFrames) {
							toDo = io.numFrames;
						}

						while (toDo) {
							auto rr = ep.second.data.Read(toDo * numCh);
							auto doNow = rr.size / numCh;

							for (auto c : ep.second.map) {
								for (unsigned i = 0;i < doNow;++i) {
									delegateIn[(i + gap) * streamIns + c.second] = rr[i * numCh + c.first];
								}
							}
							toDo -= doNow;
							gap += doNow;
						}
					}

					io.input = delegateIn.data();
				}

				void SplatOutput(PAD::IO& io) {
					for (auto &ep : out) {
						BYTE* data;
						ep.second.service->GetBuffer(io.numFrames, &data);
						float *epData = (float*)data;
						
						auto epChannels = ep.second.GetNumChannels();
						auto delChannels = cfg.GetNumStreamOutputs();

						if (delChannels < epChannels) {
							memset(epData, 0, sizeof(float)*io.numFrames*epChannels);
						}

						for (auto c : ep.second.map) {
							for (unsigned i = 0; i < io.numFrames;++i) {
								epData[i * epChannels + c.first] = delegateOut[i * delChannels + c.second];
							}
						}
						ep.second.service->ReleaseBuffer(io.numFrames, 0);
					}
				}

				void AllocateOutput(PAD::IO& io) {
					if (delegateOut.size() < io.numFrames * cfg.GetNumStreamOutputs()) {
						delegateOut.resize(io.numFrames * cfg.GetNumStreamOutputs());
					}
					io.output = delegateOut.data();
					memset(io.output, 0, sizeof(float) * io.numFrames * cfg.GetNumStreamOutputs());
				}

				STDMETHODIMP Invoke(IRtwqAsyncResult* result) override {
					RTWQWORKITEM_KEY wik;

					if (!runTask.test_and_set()) {
						SetEvent(RTWQ.Done);
						result->SetStatus(S_OK);
						return S_OK;
					}

					PAD::IO io{ cfg };
					io.numFrames = cfg.GetBufferSize();

					// how many frames?
					for (auto &ep : out) {
						UINT32 usedFrames;
						ep.first->GetCurrentPadding(&usedFrames);
						io.numFrames = std::min(io.numFrames, cfg.GetBufferSize() - usedFrames);
					}

					// pull input
					UINT64 earliestTime = 0ull;
					for (auto &ep : in) {
						UINT32 pending;
						do {
							DWORD flags = 0;
							BYTE* data = nullptr;
							UINT64 streamTime = 0, pcTime = 0;
							UINT32 frames = 0;
							ep.second.service->GetBuffer(&data, &frames, &flags, &streamTime, &pcTime);
							earliestTime = std::min(pcTime, earliestTime);
							auto numSamples = frames * ep.second.GetNumChannels();
							auto didWrite = ep.second.data.Write((const float*)data, numSamples);
							ep.second.service->ReleaseBuffer(frames);
							if (didWrite != numSamples) break;
							ep.second.service->GetNextPacketSize(&pending);
						} while (pending);

						if (io.numFrames > ep.second.data.ReadAvailable()) {
							io.numFrames = ep.second.data.ReadAvailable();
						}
					}

					if (io.numFrames) {
						// 100ns to us
						io.inputBufferTime = std::chrono::microseconds(earliestTime / 10);

						SplatInput(io);
						AllocateOutput(io);

						if (clock.Get()) {
							UINT64 streamPosBytes, pcPos, bytesPerSecond;
							clock->GetFrequency(&bytesPerSecond);
							clock->GetPosition(&streamPosBytes, &pcPos);
							std::chrono::microseconds streamPlayed(streamPosBytes * 1000'000ull / bytesPerSecond);
							std::chrono::microseconds streamRendered(rendered * 1000'000ull / (UINT64)cfg.GetSampleRate());
							auto latency = streamRendered - streamPlayed;
							std::chrono::microseconds systemTime(pcPos / 10);
							io.outputBufferTime = systemTime + latency;
						}

						dev->BufferSwitch(io);
						rendered += io.numFrames;
						SplatOutput(io);
					}

					result->SetStatus(S_OK);
					return RtwqPutWaitingWorkItem(RTWQ.Event, 1, result, &wik);
				}

				Stream(WasapiDevice* dev, const AudioStreamConfiguration& desired) :cfg(desired),dev(dev) {
					int streamChannel = 0;
					auto &dCfg(dev->Cfg());
					streaming.clear();

					cfg.SetDeviceChannelLimits((unsigned int)dCfg.inputChannel.size(), (unsigned)dCfg.outputChannel.size());

					for (auto r : cfg.GetInputRanges()) {
						for (auto c = r.begin();c != r.end();++c) {
							auto epChannel = dCfg.inputChannel[c];
							auto ep = dev->inputPorts[epChannel.first].Get();
							in[ep].map.emplace_back(epChannel.second, streamChannel++);
						}
					}

					streamChannel = 0;
					for (auto r : cfg.GetOutputRanges()) {
						for (auto c = r.begin();c != r.end();++c) {
							auto epChannel = dCfg.outputChannel[c];
							auto ep = dev->outputPorts[epChannel.first].Get();
							out[ep].map.emplace_back(epChannel.second, streamChannel++);
						}
					}

					AudioClientProperties props = { 0 };
					props.cbSize = sizeof(AudioClientProperties);
					props.eCategory = AudioCategory_Media;

					WinError::Context("Opening output endpoints");
					
					RTWQ.Event = CreateEvent(nullptr, false, false, nullptr);
					RTWQ.SecondaryEvent = CreateEvent(nullptr, false, false, nullptr);
					RTWQ.Done = CreateEvent(nullptr, false, false, nullptr);

					for (auto &iep : in) dev->InitializePort(cfg, iep.first, iep.second, RTWQ.Event);
					for (auto &oep : out) dev->InitializePort(cfg, oep.first, oep.second, in.empty() ? RTWQ.Event : RTWQ.SecondaryEvent);

					for (auto &ep : out) {
						if (ep.first->GetService(__uuidof(IAudioClock), (void**)clock.Reset()) == S_OK) break;
					}

					RTWQ.Queue = RTWQ_MULTITHREADED_WORKQUEUE;
					RTWQ.Task = 0;
					WinError::Context("Creating real time work queue");
					WinError err = RtwqStartup();
					RTWQWORKITEM_KEY wik;
					runTask.test_and_set(); 
					err = RtwqCreateAsyncResult(nullptr, this, nullptr, RTWQ.Result.Reset());
					err = RtwqLockSharedWorkQueue(L"Audio", 0, &RTWQ.Task, &RTWQ.Queue);

					// wait on input first, output subsequently
					err = RtwqPutWaitingWorkItem(RTWQ.Event, 2, RTWQ.Result.Get(), &wik);

					if (cfg.HasSuspendOnStartup() == false) Activate(true);
				}

				~Stream() {
					Activate(false);
					runTask.clear();
					WaitForSingleObject(RTWQ.Done, 1000);
					CloseHandle(RTWQ.Done);
					CloseHandle(RTWQ.Event);
					CloseHandle(RTWQ.SecondaryEvent);
					RtwqUnlockWorkQueue(RTWQ.Queue);
					RtwqShutdown();
				}

				virtual void Activate(bool onOff) {
					if (onOff) {
						if (!streaming.test_and_set()) {
							dev->AboutToBeginStream(cfg);
							for (auto &ep : in) ep.first->Start();
							for (auto &ep : out) ep.first->Start();
						}
					} else {
						if (streaming.test_and_set()) {
							for (auto &ep : in) ep.first->Stop();
							for (auto &ep : out) ep.first->Stop();
							streaming.clear();
						}
					}
				}

				virtual const AudioStreamConfiguration& Config() const {
					return cfg;
				}
			};

			stream = std::make_unique<Stream>(this, cfg);
			return stream->Config();
		}

		void Resume() {
			if (!stream) throw PAD::SoftError(PAD::DeviceStartStreamFailure, "No stream to resume");
			stream->Activate(true);
		}

		void Suspend() {
			if (!stream) throw PAD::SoftError(PAD::DeviceStopStreamFailure, "No stream to suspend");
			stream->Activate(false);
		}

		void Close() {
			stream.reset();
		}

		double CPU_Load() const {
			return 0.0;
		}

		std::chrono::microseconds DeviceTimeNow() const {
			LARGE_INTEGER pc, pcFreq;
			QueryPerformanceCounter(&pc);
			QueryPerformanceFrequency(&pcFreq);
			return std::chrono::microseconds(pc.QuadPart * 1000'000ull / pcFreq.QuadPart);
		}
	};

	struct Publisher : public HostAPIPublisher {
		using DeviceMapTy = std::unordered_map<std::string, std::unique_ptr<WasapiDevice>>;
		DeviceMapTy devices;
		std::unique_ptr<WasapiDevice> defaultDevice;

		void Enumerate() {
			WinError err;
			WinError::Context("Enumerating devices");
			auto deviceEnumerator = ComRef<IMMDeviceEnumerator>::Make(ID::DeviceEnumerator, CLSCTX_ALL);

			auto devColl = deviceEnumerator.GetObject(
				&IMMDeviceEnumerator::EnumAudioEndpoints, eAll, DEVICE_STATE_ACTIVE);

			UINT numEndpoints(0);
			err = devColl->GetCount(&numEndpoints);

			for (UINT i = 0;i < numEndpoints;++i) {
				WinError::Context("Retrieving audio endpoint properties");

				auto epDevice = devColl.GetObject(&IMMDeviceCollection::Item, i);
				auto epProps =  epDevice.GetObject(&IMMDevice::OpenPropertyStore, STGM_READ);

				PROPVARIANT adapterName, endpointName;
				err = epProps->GetValue(PKEY_DeviceInterface_FriendlyName, &adapterName);
				err = epProps->GetValue(PKEY_Device_DeviceDesc, &endpointName);
				auto deviceName = toUtf8(adapterName.pwszVal);

				if (devices.count(deviceName) == 0) {
					devices.emplace(deviceName, std::make_unique<WasapiDevice>(deviceName));
				}

				auto ep = epDevice.Query<IMMEndpoint>();
				EDataFlow flow; ep->GetDataFlow(&flow);

				auto epClient = ComObject<IAudioClient>(&IMMDevice::Activate, 
					epDevice.Get(), __uuidof(IAudioClient), CLSCTX_ALL, nullptr);

				switch (flow) {
				case eRender:
					devices[deviceName]->outputPorts.emplace_back(std::move(epClient));
					break;
				case eCapture:
					devices[deviceName]->inputPorts.emplace_back(std::move(epClient));
					break;
				}
			}

			defaultDevice = std::make_unique<WasapiDevice>("System");

			auto defaultOut = deviceEnumerator.GetObject(
				&IMMDeviceEnumerator::GetDefaultAudioEndpoint, eRender, eMultimedia);
			auto defaultIn = deviceEnumerator.GetObject(
				&IMMDeviceEnumerator::GetDefaultAudioEndpoint, eCapture, eMultimedia);			

			defaultDevice->outputPorts.emplace_back(
				ComObject<IAudioClient>(&IMMDevice::Activate, defaultOut.Get(), 
										__uuidof(IAudioClient), CLSCTX_ALL, nullptr));
			defaultDevice->inputPorts.emplace_back(
				ComObject<IAudioClient>(&IMMDevice::Activate, defaultIn.Get(),
										__uuidof(IAudioClient), CLSCTX_ALL, nullptr));
		}

		void Configure() {
			defaultDevice->Configure();
			for (auto &dev : devices) {
				dev.second->Configure();
			}
		}

	public:

		void Publish(Session& PAD, DeviceErrorDelegate& errors) {
			CoInitialize(nullptr);
			Enumerate();
			Configure();
			PAD.Register(defaultDevice.get());
			for (auto &dev : devices) {
				PAD.Register(dev.second.get());
			}
		}

		const char* GetName() const {
			return "WASAPI";
		}

	} publisher;
}

namespace PAD {
	IHostAPI* LinkWASAPI() {
		return &publisher;
	}
}
