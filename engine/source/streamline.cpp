#include "pch.h"
#include "streamline.h"
#include "debug_console.h"

#ifdef UPDOWN_STREAMLINE
#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_helpers.h>
#include <sl_security.h>
#endif

namespace udsdx
{
#ifdef UPDOWN_STREAMLINE
	namespace
	{
		// Streamline resolves plugins relative to the executable, which is where CMake stages
		// them, so no explicit search path is needed. The interposer itself must be found the same
		// way: LoadLibraryW with a bare name would consult the whole DLL search order, and an
		// sl.interposer.dll picked up from somewhere else on the system is exactly the kind of
		// substitution the signature check exists to stop.
		std::wstring GetExecutableDirectory()
		{
			wchar_t path[MAX_PATH] = {};
			const DWORD length = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
			if (length == 0 || length == MAX_PATH)
			{
				return {};
			}

			std::wstring result(path, length);
			const size_t separator = result.find_last_of(L'\\');
			return separator == std::wstring::npos ? std::wstring{} : result.substr(0, separator);
		}

		template <typename T>
		bool ResolveProc(HMODULE module, const char* name, T& out)
		{
			out = reinterpret_cast<T>(::GetProcAddress(module, name));
			return out != nullptr;
		}
	}
#endif

	Streamline::~Streamline()
	{
		Shutdown();
	}

	void Streamline::Initialize()
	{
#ifndef UPDOWN_STREAMLINE
		// UPDOWN_ENABLE_STREAMLINE=OFF: the default status message already says so.
		return;
#else
		const std::wstring directory = GetExecutableDirectory();
		if (directory.empty())
		{
			m_statusMessage = "could not resolve the executable directory";
			DebugConsole::LogError("Streamline: " + m_statusMessage);
			return;
		}

		const std::wstring interposerPath = directory + L"\\sl.interposer.dll";

		// Streamline ships signed production binaries and unsigned development ones. Refusing the
		// unsigned set here rather than disabling the check keeps the shipping configuration and
		// the development one on the same code path.
		if (!sl::security::verifyEmbeddedSignature(interposerPath.c_str()))
		{
			m_statusMessage = "sl.interposer.dll is missing or not signed by NVIDIA";
			DebugConsole::Log("Streamline: " + m_statusMessage + ", DLSS is unavailable");
			return;
		}

		m_interposer = ::LoadLibraryW(interposerPath.c_str());
		if (m_interposer == nullptr)
		{
			m_statusMessage = "sl.interposer.dll failed to load";
			DebugConsole::LogError("Streamline: " + m_statusMessage);
			return;
		}

		PFun_slInit* slInitFn = nullptr;
		PFun_slShutdown* slShutdownFn = nullptr;
		PFun_slIsFeatureSupported* slIsFeatureSupportedFn = nullptr;
		PFun_slSetD3DDevice* slSetD3DDeviceFn = nullptr;

		if (!ResolveProc(m_interposer, "slInit", slInitFn)
			|| !ResolveProc(m_interposer, "slShutdown", slShutdownFn)
			|| !ResolveProc(m_interposer, "slIsFeatureSupported", slIsFeatureSupportedFn)
			|| !ResolveProc(m_interposer, "slSetD3DDevice", slSetD3DDeviceFn))
		{
			m_statusMessage = "sl.interposer.dll is missing expected exports";
			DebugConsole::LogError("Streamline: " + m_statusMessage);
			::FreeLibrary(m_interposer);
			m_interposer = nullptr;
			return;
		}

		// Only the denoiser is requested. Asking for features that will never be evaluated would
		// load their plugins and their NGX back ends for nothing.
		static const sl::Feature featuresToLoad[] = { sl::kFeatureDLSS_RR };

		sl::Preferences preferences{};
		preferences.featuresToLoad = featuresToLoad;
		preferences.numFeaturesToLoad = static_cast<uint32_t>(std::size(featuresToLoad));
		preferences.renderAPI = sl::RenderAPI::eD3D12;

		// Application identity, without which NGX refuses to initialize and every NGX-backed
		// feature -- Ray Reconstruction included -- unloads itself at plugin startup.
		//
		// SL accepts identity in one of two forms: an application id issued by NVIDIA, or an
		// engine/version/project triple. It picks the second whenever BOTH engineVersion and
		// projectId are non-empty, and rejects the first outright in production builds unless the
		// id is a real one. A custom engine has no NVIDIA-issued id, so the triple is the path
		// that works, and the GUID below just has to be stable across runs of this project.
		preferences.engine = sl::EngineType::eCustom;
		preferences.engineVersion = "1.0.0";
		preferences.projectId = "846017cd-844a-4079-a984-203a0e5613dc";
		// eUseManualHooking is what makes SL wait to be handed the device instead of expecting to
		// have intercepted its creation. Without it SL assumes the interposer is linked and never
		// sees a device at all.
		preferences.flags |= sl::PreferenceFlags::eUseManualHooking;
#if defined(DEBUG) || defined(_DEBUG)
		preferences.logLevel = sl::LogLevel::eVerbose;
#else
		preferences.logLevel = sl::LogLevel::eOff;
#endif
		// Logging to a file is off; messages arrive through the callback instead so they land in
		// the engine's own console alongside everything else.
		preferences.pathToLogsAndData = nullptr;
		preferences.logMessageCallback = [](sl::LogType type, const char* message)
		{
			if (type == sl::LogType::eError)
			{
				DebugConsole::LogError(std::string("Streamline: ") + message);
			}
			else
			{
				DebugConsole::Log(std::string("Streamline: ") + message);
			}
		};

		const sl::Result result = slInitFn(preferences, sl::kSDKVersion);
		if (result != sl::Result::eOk)
		{
			m_statusMessage = std::string("slInit failed: ") + sl::getResultAsStr(result);
			DebugConsole::Log("Streamline: " + m_statusMessage);
			::FreeLibrary(m_interposer);
			m_interposer = nullptr;
			return;
		}

		m_slShutdown = reinterpret_cast<void*>(slShutdownFn);
		m_slIsFeatureSupported = reinterpret_cast<void*>(slIsFeatureSupportedFn);
		m_slSetD3DDevice = reinterpret_cast<void*>(slSetD3DDeviceFn);
		m_initialized = true;
		m_statusMessage = "initialized, adapter not queried yet";
		DebugConsole::Log("Streamline: initialized (SDK " + std::to_string(sl::kSDKVersion) + ")");
#endif
	}

	void Streamline::OnDeviceCreated(ID3D12Device* device, IDXGIAdapter1* adapter)
	{
#ifndef UPDOWN_STREAMLINE
		(void)device;
		(void)adapter;
#else
		if (!m_initialized || device == nullptr || adapter == nullptr)
		{
			return;
		}

		auto* slSetD3DDeviceFn = reinterpret_cast<PFun_slSetD3DDevice*>(m_slSetD3DDevice);
		const sl::Result deviceResult = slSetD3DDeviceFn(device);
		if (deviceResult != sl::Result::eOk)
		{
			m_statusMessage = std::string("slSetD3DDevice failed: ") + sl::getResultAsStr(deviceResult);
			DebugConsole::LogError("Streamline: " + m_statusMessage);
			return;
		}

		DXGI_ADAPTER_DESC1 desc = {};
		if (FAILED(adapter->GetDesc1(&desc)))
		{
			m_statusMessage = "could not read the adapter description";
			DebugConsole::LogError("Streamline: " + m_statusMessage);
			return;
		}

		// slIsFeatureSupported matches on the adapter LUID, so support is answered per physical
		// GPU rather than per process -- correct on a laptop with a second, non-RTX adapter.
		sl::AdapterInfo adapterInfo{};
		adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
		adapterInfo.deviceLUIDSizeInBytes = sizeof(desc.AdapterLuid);

		auto* slIsFeatureSupportedFn = reinterpret_cast<PFun_slIsFeatureSupported*>(m_slIsFeatureSupported);
		const sl::Result supportResult = slIsFeatureSupportedFn(sl::kFeatureDLSS_RR, adapterInfo);
		m_rayReconstructionSupported = supportResult == sl::Result::eOk;

		if (m_rayReconstructionSupported)
		{
			m_statusMessage.clear();
			DebugConsole::Log("Streamline: DLSS Ray Reconstruction is supported on this adapter");
		}
		else
		{
			m_statusMessage = sl::getResultAsStr(supportResult);
			DebugConsole::Log("Streamline: DLSS Ray Reconstruction unsupported (" + m_statusMessage + ")");
		}
#endif
	}

	void Streamline::Shutdown()
	{
#ifdef UPDOWN_STREAMLINE
		if (m_initialized)
		{
			auto* slShutdownFn = reinterpret_cast<PFun_slShutdown*>(m_slShutdown);
			slShutdownFn();
			m_initialized = false;
		}

		if (m_interposer != nullptr)
		{
			::FreeLibrary(m_interposer);
			m_interposer = nullptr;
		}
#endif
		m_rayReconstructionSupported = false;
		m_slShutdown = nullptr;
		m_slIsFeatureSupported = nullptr;
		m_slSetD3DDevice = nullptr;
	}
}
