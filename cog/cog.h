// Component Object Golf
// COM is over-engineered. That means we can be too!

#pragma once

#include <codecvt>
#include <string>
#include <memory>
#include <functional>

namespace COG {
	namespace detail {
		template <typename U> struct ArgDeducer {};
		template <typename... ARGS> struct ReturnObject {};

		template <typename L> struct ReturnObject<L**> {
			using type = L;
		};

		template <typename A, typename B, typename... REST> struct ReturnObject<A, B, REST...> {
			using type = typename ReturnObject<B, REST...>::type;
		};

		template <typename OBJ, typename... ARGS> struct ArgDeducer<HRESULT (__stdcall OBJ::*)(ARGS...)> {
			using ReturnObjectTy = typename ReturnObject<ARGS...>::type;
		};

		template <typename... ARGS> struct ArgDeducer<HRESULT (__stdcall *)(ARGS...)> {
			using ReturnObjectTy = typename ReturnObject<ARGS...>::type;
		};
	}

	static std::string toUtf8(std::wstring ucs) {
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

		bool IsError(std::string& explain) {
			if (err != S_OK) {
				explain = Format(err);
				return true;
			}
			return false;
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

		ComRef(const ComRef& src) :ptr(src.ptr) {
			if (ptr) {
				ptr->AddRef();
			}
		}

		ComRef(ComRef&& src) {
			std::swap(ptr, src.ptr);
		}

		~ComRef() {
			if (ptr) {
				ptr->Release();
			}
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
			auto err = CoCreateInstance(rclsid, nullptr, context, __uuidof(T), (void**)Reset());
			if (ptr) ptr->AddRef();
			return err;
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
			using ObjTy = typename detail::ArgDeducer<MEMFN>::ReturnObjectTy;
			auto comFn = std::bind(memfn, _1, std::forward<STUFF>(stuff)..., _2);
			ComRef<ObjTy> tmp;
			WinError err = comFn(ptr, tmp.Reset());
			return tmp;
		}

		T* operator->() const {
			return ptr;
		}

		T* Get() const {
			return ptr;
		}
	};

	using namespace detail;

	template <typename T> using CoTaskPointer = std::unique_ptr<T, decltype(&CoTaskMemFree)>;

	// all COM functions take only scalar  POD parameters, so forwarding is a waste of keystrokes.
	// much like this comment.
	template <typename T, typename... STUFF> CoTaskPointer<T> GetCoTaskMem(STUFF... stuff) {
		auto comFn = std::bind(stuff..., std::placeholders::_1);
		T* raw = nullptr;
		WinError err = comFn(&raw);
		return CoTaskPointer<T>(raw, CoTaskMemFree);
	}

	template <typename T, typename... STUFF> auto TransferObjectOwnership(STUFF... stuff) {
		auto comFn = std::bind(stuff..., std::placeholders::_1);
		ComRef<T> ref;
		WinError err = comFn((LPVOID*)ref.Reset());
		return ref;
	}

	template <typename T, typename... STUFF> auto GetAndRetainObject(STUFF... stuff) {
		auto ref = TransferObjectOwnership<T, STUFF...>(std::forward<STUFF>(stuff)...);
		ref->AddRef();
		return ref;
	}

	template <typename INTERFACE>
	struct TemporaryObject : public INTERFACE  {
		STDMETHOD_(ULONG, AddRef)() throw() { return 0; }
		STDMETHOD_(ULONG, Release)() throw() { return 0; }
		STDMETHOD(QueryInterface)(REFIID, _COM_Outptr_ void** ppvObject) throw() {
			*ppvObject = NULL;
			return E_NOINTERFACE;
		}
	};

	struct Holder {
		Holder(tagCOINIT flags = COINIT_MULTITHREADED) {
			CoInitializeEx(nullptr, flags);
		}

		~Holder() {
			//CoUninitialize();
		}

		Holder(const Holder&) = delete;
		Holder& operator=(const Holder&) = delete;
	};

	using Handle = std::unique_ptr<Holder>;
}