#pragma comment(lib,"Ole32.lib")
#pragma comment(lib,"Winmm.lib")

#define NO_MINMAX
#include "pad.h"
#include "HostAPI.h"

#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <Avrt.h>
#include <Functiondiscoverykeys_devpkey.h>

#include <codecvt>
#include <string.h>
#include <unordered_map>
#include <iostream> // temp

#include "WinDebugStream.h"

namespace {
	namespace ID {
		const CLSID DeviceEnumerator = __uuidof(::MMDeviceEnumerator);
	}

	namespace detail {
		template <typename U> struct ArgDeducer {};
		template <typename... ARGS> struct LastArg {};

		template <typename L> struct LastArg<L**> {
			using type = L;
		};

		template <typename A, typename B, typename... REST> struct LastArg<A, B, REST...> {
			using type = typename LastArg<B, REST...>::type;
		};

		template <typename OBJ, typename... ARGS> struct ArgDeducer<HRESULT(OBJ::*)(ARGS...)> {
			using LastArgTy = typename LastArg<ARGS...>::type;
		};
	}

	std::string toUtf8(std::wstring ucs) {
		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t> cvt;
		return cvt.to_bytes(ucs);
	}

	struct WinError {
		HRESULT err;
		WinError(HRESULT from = S_OK) :err(from) {}
		~WinError() noexcept(false) {
			if (err != S_OK) {
				throw std::runtime_error(Context() + Format(err));
			}
		}

		static const char *Context(const char *upd = nullptr) {
			static thread_local const char *context = "Unknown context";
			if (upd) context = upd;
			return context;
		}

		WinError& operator=(WinError tmp) {
			std::swap(err, tmp.err);
			return *this;
		}

		HRESULT Clear() {
			auto tmp = err;
			err = S_OK;
			return tmp;
		}

		static std::string Format(HRESULT err) {
			LPTSTR errorText = nullptr;
			FormatMessage(
				FORMAT_MESSAGE_FROM_SYSTEM |
				FORMAT_MESSAGE_ALLOCATE_BUFFER |
				FORMAT_MESSAGE_IGNORE_INSERTS,
				nullptr,
				err,
				MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				(LPTSTR)&errorText,
				0,
				nullptr);

			if (errorText) {
				auto wstr = std::wstring(errorText, errorText + wcslen(errorText));
				HeapFree(GetProcessHeap(), 0, errorText);
				return toUtf8(wstr);
			} else {
				return std::to_string(err);
			}
		}

		HRESULT Ignore(const std::initializer_list<HRESULT>& ignore) {
			for (auto &i : ignore) {
				if (err == i) {
					return Clear();
				}
			}
			return S_OK;
		}
	};

	template <typename T> class ComRef {
		T* ptr = nullptr;
	public:
		ComRef() {}

		ComRef(ComRef& src) :ptr(src.ptr) {
			if (ptr) ptr->AddRef();
		}

		~ComRef() {
			if (ptr) ptr->Release();
		}

		ComRef(ComRef&& src) {
			std::swap(ptr, src.ptr);
		}

		ComRef& operator=(ComRef src) {
			std::swap(ptr, src.ptr);
			return *this;
		}

		T** Reset() {
			*this = ComRef();
			return &ptr;
		}

		WinError Instantiate(REFCLSID rclsid, DWORD context) {
			return CoCreateInstance(rclsid, nullptr, context, __uuidof(T), (void**)Reset());
		}

		static ComRef Make(REFCLSID rclsid, DWORD context) {
			ComRef tmp;	tmp.Instantiate(rclsid, context); return tmp;
		}

		template <typename INTERFACE>
		ComRef<INTERFACE> Query() const {
			ComRef<INTERFACE> iif;
			if (ptr) {
				ptr->QueryInterface(__uuidof(INTERFACE), (void**)iif.Reset());
			}
			return iif;
		}

		template <typename MEMFN, typename... STUFF> auto GetObject(MEMFN memfn, STUFF&&... stuff) {
			using namespace std::placeholders;
			using ObjTy = typename detail::ArgDeducer<MEMFN>::LastArgTy;
			auto comFn = std::bind(memfn, _1, std::forward<STUFF>(stuff)..., _2);
			ComRef<ObjTy> tmp;
			WinError err = comFn(ptr, tmp.Reset());
			return tmp;
		}

		const T* operator->() const {
			return ptr;
		}

		T* operator->() {
			return ptr;
		}

		T* Get() {
			return ptr;
		}
	};

	using namespace detail;

	template <typename T> using CoTaskPointer = std::unique_ptr<T, decltype(&CoTaskMemFree)>;

	template <typename T, typename... STUFF> CoTaskPointer<T> GetCoTaskMem(STUFF&&... stuff) {
		auto comFn = std::bind(std::forward<STUFF>(stuff)..., std::placeholders::_1);
		T* raw = nullptr;
		WinError err = comFn(&raw);
		return CoTaskPointer<T>(raw, CoTaskMemFree);
	}

	template <typename T, typename... STUFF> ComRef<T> ComObject(STUFF&&... stuff) {
		auto comFn = std::bind(std::forward<STUFF>(stuff)..., std::placeholders::_1);
		ComRef<T> ref;
		WinError err = comFn((void**)ref.Reset());
		return ref;
	}


	using namespace PAD;
	class WasapiDevice : public AudioDevice {
	public:
		using AudioClientRef = ComRef<IAudioClient>;
		std::vector<AudioClientRef> inputPorts, outputPorts;
		std::string name;

		WasapiDevice(std::string name) :name(name) {
		}

		unsigned int GetNumInputs() const {
			return 0;
		}

		unsigned int GetNumOutputs() const {
			return 0;
		}

		const char *GetName() const {
			return name.c_str();
		}

		const char *GetHostAPI() const {
			return "WASAPI";
		}

		bool Supports(const AudioStreamConfiguration& cfg) const {
			return false;
		}

		AudioStreamConfiguration DefaultMono() const {
			return {};
		}

		AudioStreamConfiguration DefaultStereo() const {
			return {};
		}

		AudioStreamConfiguration DefaultAllChannels() const {
			return {};
		}

		const AudioStreamConfiguration& Open(const AudioStreamConfiguration& cfg) {
			return {};
		}

		void Resume() {

		}

		void Suspend() {

		}

		void Close() {

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

	class Publisher : public HostAPIPublisher {
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
				ComObject<IAudioClient>(&IMMDevice::Activate, defaultIn.Get(), 
										__uuidof(IAudioClient), CLSCTX_ALL, nullptr));
			defaultDevice->inputPorts.emplace_back(
				ComObject<IAudioClient>(&IMMDevice::Activate, defaultOut.Get(),
										__uuidof(IAudioClient), CLSCTX_ALL, nullptr));
		}
	public:

		void Publish(Session& PAD, DeviceErrorDelegate& errors) {
			CoInitialize(nullptr);
			Enumerate();
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
