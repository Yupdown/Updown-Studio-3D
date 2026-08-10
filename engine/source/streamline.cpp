#include "pch.h"
#include "streamline.h"
#include "debug_console.h"

#ifdef UPDOWN_STREAMLINE
#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_dlss_d.h>
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

		// Streamline and the engine agree on matrix convention -- row-major storage, row vectors,
		// translation in the last row -- so this is a straight copy. It is deliberately NOT the
		// transpose the HLSL constant buffer upload applies; passing a transposed matrix here
		// produces a denoiser that reprojects into nonsense while everything still renders.
		sl::float4x4 ToStreamline(const Matrix4x4& m)
		{
			sl::float4x4 result{};
			result.setRow(0, { m._11, m._12, m._13, m._14 });
			result.setRow(1, { m._21, m._22, m._23, m._24 });
			result.setRow(2, { m._31, m._32, m._33, m._34 });
			result.setRow(3, { m._41, m._42, m._43, m._44 });
			return result;
		}

		sl::float2 ToStreamline(const Vector2& v) { return { v.x, v.y }; }
		sl::float3 ToStreamline(const Vector3& v) { return { v.x, v.y, v.z }; }

		// One viewport: this engine drives a single DLSS instance. Not constexpr because the
		// handle derives from sl::BaseStructure, whose constructor is not.
		const sl::ViewportHandle kViewport{ 0u };
	}

	// Entry points resolved from sl.interposer.dll. Dynamic loading means the inline helpers in
	// sl_dlss_d.h are unusable -- they call a global slGetFeatureFunction that was never linked --
	// so the feature functions are resolved by hand through the one we did resolve.
	struct Streamline::Functions
	{
		PFun_slShutdown* Shutdown = nullptr;
		PFun_slIsFeatureSupported* IsFeatureSupported = nullptr;
		PFun_slSetD3DDevice* SetD3DDevice = nullptr;
		PFun_slUpgradeInterface* UpgradeInterface = nullptr;
		PFun_slGetFeatureFunction* GetFeatureFunction = nullptr;
		PFun_slGetNewFrameToken* GetNewFrameToken = nullptr;
		PFun_slSetTagForFrame* SetTagForFrame = nullptr;
		PFun_slSetConstants* SetConstants = nullptr;
		PFun_slEvaluateFeature* EvaluateFeature = nullptr;
		PFun_slFreeResources* FreeResources = nullptr;

		PFun_slDLSSDSetOptions* DLSSDSetOptions = nullptr;

		sl::FrameToken* FrameToken = nullptr;
	};
#else
	struct Streamline::Functions {};
#endif

	Streamline::Streamline() = default;

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

		auto functions = std::make_unique<Functions>();
		PFun_slInit* slInitFn = nullptr;

		if (!ResolveProc(m_interposer, "slInit", slInitFn)
			|| !ResolveProc(m_interposer, "slShutdown", functions->Shutdown)
			|| !ResolveProc(m_interposer, "slIsFeatureSupported", functions->IsFeatureSupported)
			|| !ResolveProc(m_interposer, "slSetD3DDevice", functions->SetD3DDevice)
			|| !ResolveProc(m_interposer, "slUpgradeInterface", functions->UpgradeInterface)
			|| !ResolveProc(m_interposer, "slGetFeatureFunction", functions->GetFeatureFunction)
			|| !ResolveProc(m_interposer, "slGetNewFrameToken", functions->GetNewFrameToken)
			|| !ResolveProc(m_interposer, "slSetTagForFrame", functions->SetTagForFrame)
			|| !ResolveProc(m_interposer, "slSetConstants", functions->SetConstants)
			|| !ResolveProc(m_interposer, "slEvaluateFeature", functions->EvaluateFeature)
			|| !ResolveProc(m_interposer, "slFreeResources", functions->FreeResources))
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
		// sees a device at all. eUseFrameBasedResourceTagging opts into slSetTagForFrame; the
		// frame-less slSetTag it replaces is deprecated.
		preferences.flags |= sl::PreferenceFlags::eUseManualHooking;
		preferences.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
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

		m_functions = std::move(functions);
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

		const sl::Result deviceResult = m_functions->SetD3DDevice(device);
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

		const sl::Result supportResult = m_functions->IsFeatureSupported(sl::kFeatureDLSS_RR, adapterInfo);
		m_rayReconstructionSupported = supportResult == sl::Result::eOk;

		if (!m_rayReconstructionSupported)
		{
			m_statusMessage = sl::getResultAsStr(supportResult);
			DebugConsole::Log("Streamline: DLSS Ray Reconstruction unsupported (" + m_statusMessage + ")");
			return;
		}

		// Resolved now rather than at first use: a missing feature function means the plugin did
		// not really load, and finding that out mid-frame is worse than finding it out here.
		void* setOptions = nullptr;
		if (m_functions->GetFeatureFunction(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", setOptions) != sl::Result::eOk
			|| setOptions == nullptr)
		{
			m_rayReconstructionSupported = false;
			m_statusMessage = "slDLSSDSetOptions is unavailable";
			DebugConsole::LogError("Streamline: " + m_statusMessage);
			return;
		}
		m_functions->DLSSDSetOptions = reinterpret_cast<PFun_slDLSSDSetOptions*>(setOptions);

		m_statusMessage.clear();
		DebugConsole::Log("Streamline: DLSS Ray Reconstruction is supported on this adapter");
#endif
	}

	bool Streamline::UpgradeInterface(void** iface)
	{
#ifndef UPDOWN_STREAMLINE
		(void)iface;
		return false;
#else
		if (!m_initialized || iface == nullptr || *iface == nullptr)
		{
			return false;
		}

		const sl::Result result = m_functions->UpgradeInterface(iface);
		if (result != sl::Result::eOk)
		{
			DebugConsole::LogWarning(std::string("Streamline: slUpgradeInterface failed: ")
				+ sl::getResultAsStr(result));
			return false;
		}
		return true;
#endif
	}

	void Streamline::BeginFrame(UINT frameIndex)
	{
#ifndef UPDOWN_STREAMLINE
		(void)frameIndex;
#else
		if (!m_initialized)
		{
			return;
		}

		const uint32_t index = static_cast<uint32_t>(frameIndex);
		if (m_functions->GetNewFrameToken(m_functions->FrameToken, &index) != sl::Result::eOk)
		{
			m_functions->FrameToken = nullptr;
		}
#endif
	}

	bool Streamline::SetRayReconstructionOptions(UINT width, UINT height)
	{
#ifndef UPDOWN_STREAMLINE
		(void)width;
		(void)height;
		return false;
#else
		if (!m_rayReconstructionSupported || width == 0 || height == 0)
		{
			return false;
		}

		if (m_optionsValid && m_configuredWidth == width && m_configuredHeight == height)
		{
			return true;
		}

		// A size change reinitialises the denoiser internally, so drop what the previous size
		// allocated first rather than leaving a viewport's worth of VRAM behind per resize.
		if (m_optionsValid)
		{
			FreeRayReconstructionResources();
		}

		sl::DLSSDOptions options{};
		// DLAA: input and output are the same size. Upscaling would mean splitting render and
		// display resolution across every buffer and post pass, which this engine does not do.
		options.mode = sl::DLSSMode::eDLAA;
		options.outputWidth = width;
		options.outputHeight = height;
		// Mandatory for Ray Reconstruction, which only accepts an HDR pipeline.
		options.colorBuffersHDR = sl::Boolean::eTrue;
		// Linear roughness rides in the alpha channel of the normal texture.
		options.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;

		const sl::Result result = m_functions->DLSSDSetOptions(kViewport, options);
		if (result != sl::Result::eOk)
		{
			DebugConsole::LogError(std::string("Streamline: slDLSSDSetOptions failed: ")
				+ sl::getResultAsStr(result));
			m_optionsValid = false;
			return false;
		}

		m_optionsValid = true;
		m_configuredWidth = width;
		m_configuredHeight = height;
		return true;
#endif
	}

	bool Streamline::EvaluateRayReconstruction(ID3D12GraphicsCommandList* commandList, const RayReconstructionFrame& frame)
	{
#ifndef UPDOWN_STREAMLINE
		(void)commandList;
		(void)frame;
		return false;
#else
		if (!m_rayReconstructionSupported || !m_optionsValid || commandList == nullptr
			|| m_functions->FrameToken == nullptr)
		{
			return false;
		}

		const sl::Extent extent{ 0u, 0u, frame.Width, frame.Height };
		const uint32_t inputState = static_cast<uint32_t>(frame.InputState);

		// Manual hooking means SL never saw these resources created and cannot track their states,
		// so each tag carries the state the resource will actually be in when SL uses it.
		sl::Resource noisyColor{ sl::ResourceType::eTex2d, frame.NoisyColor, inputState };
		sl::Resource output{ sl::ResourceType::eTex2d, frame.Output, static_cast<uint32_t>(frame.OutputState) };
		sl::Resource linearDepth{ sl::ResourceType::eTex2d, frame.LinearDepth, inputState };
		sl::Resource motionVectors{ sl::ResourceType::eTex2d, frame.MotionVectors, inputState };
		sl::Resource albedo{ sl::ResourceType::eTex2d, frame.Albedo, inputState };
		sl::Resource specularAlbedo{ sl::ResourceType::eTex2d, frame.SpecularAlbedo, inputState };
		sl::Resource normalRoughness{ sl::ResourceType::eTex2d, frame.NormalRoughness, inputState };

		// eValidUntilEvaluate: nothing here is touched again between tagging and the evaluate call
		// a few lines later, which lets SL read the originals instead of copying them.
		const sl::ResourceLifecycle lifecycle = sl::ResourceLifecycle::eValidUntilEvaluate;
		sl::ResourceTag tags[] = {
			sl::ResourceTag{ &noisyColor,      sl::kBufferTypeScalingInputColor,  lifecycle, &extent },
			sl::ResourceTag{ &output,          sl::kBufferTypeScalingOutputColor, lifecycle, &extent },
			sl::ResourceTag{ &linearDepth,     sl::kBufferTypeLinearDepth,        lifecycle, &extent },
			sl::ResourceTag{ &motionVectors,   sl::kBufferTypeMotionVectors,      lifecycle, &extent },
			sl::ResourceTag{ &albedo,          sl::kBufferTypeAlbedo,             lifecycle, &extent },
			sl::ResourceTag{ &specularAlbedo,  sl::kBufferTypeSpecularAlbedo,     lifecycle, &extent },
			sl::ResourceTag{ &normalRoughness, sl::kBufferTypeNormalRoughness,    lifecycle, &extent },
		};

		sl::Result result = m_functions->SetTagForFrame(*m_functions->FrameToken, kViewport,
			tags, static_cast<uint32_t>(std::size(tags)), commandList);
		if (result != sl::Result::eOk)
		{
			DebugConsole::LogError(std::string("Streamline: slSetTagForFrame failed: ")
				+ sl::getResultAsStr(result));
			return false;
		}

		sl::Constants constants{};
		constants.cameraViewToClip = ToStreamline(frame.ViewToClip);
		constants.clipToCameraView = ToStreamline(frame.ClipToView);
		constants.clipToPrevClip = ToStreamline(frame.ClipToPrevClip);
		constants.prevClipToClip = ToStreamline(frame.PrevClipToClip);
		constants.jitterOffset = ToStreamline(frame.JitterOffset);
		// NGX wants the vector pointing at where the pixel WAS: previousUV - currentUV, in pixels.
		// Streamline's own conversion kernel makes that explicit -- when it computes motion itself
		// it writes -(uvCurrent - uvPrevious) * size and then sets MV_Scale to 1. On this path SL
		// passes the buffer straight through with MV_Scale = mvecScale * renderSize, so the sign
		// flip has to come from here: the buffer holds currentUV - previousUV.
		//
		// Getting this backwards is close to invisible under rotation, which is why it survived
		// the first round of checks. A uniform error puts every pixel's history in the wrong place
		// at once and the denoiser simply rejects all of it. Under translation the motion field is
		// radial, so the error goes to zero at the focus of expansion and grows outward -- near the
		// focus it is small enough to pass validation, and the mistake compounds every frame into
		// trails that stream away from the direction of travel.
		constants.mvecScale = { -1.0f, -1.0f };
		// Defaults to INVALID_FLOAT and SL warns about it every frame, despite the header calling
		// it optional. No pinhole offset here.
		constants.cameraPinholeOffset = { 0.0f, 0.0f };
		constants.cameraPos = ToStreamline(frame.CameraPosition);
		constants.cameraUp = ToStreamline(frame.CameraUp);
		constants.cameraRight = ToStreamline(frame.CameraRight);
		constants.cameraFwd = ToStreamline(frame.CameraForward);
		constants.cameraNear = frame.CameraNear;
		constants.cameraFar = frame.CameraFar;
		constants.cameraFOV = frame.CameraFovY;
		constants.cameraAspectRatio = frame.CameraAspectRatio;
		// View-space Z grows with distance, so depth is not inverted here even though the raster
		// path's projection is reverse-Z.
		constants.depthInverted = sl::Boolean::eFalse;
		// Camera motion is baked into the buffer. This also keeps SL from running its own motion
		// conversion pass, which would reinterpret the linear depth above as hardware depth.
		constants.cameraMotionIncluded = sl::Boolean::eTrue;
		constants.motionVectors3D = sl::Boolean::eFalse;
		constants.motionVectorsDilated = sl::Boolean::eFalse;
		constants.motionVectorsJittered = sl::Boolean::eFalse;
		constants.orthographicProjection = sl::Boolean::eFalse;
		constants.reset = frame.ResetHistory ? sl::Boolean::eTrue : sl::Boolean::eFalse;

		result = m_functions->SetConstants(constants, *m_functions->FrameToken, kViewport);
		if (result != sl::Result::eOk)
		{
			DebugConsole::LogError(std::string("Streamline: slSetConstants failed: ")
				+ sl::getResultAsStr(result));
			return false;
		}

		const sl::BaseStructure* inputs[] = { &kViewport };
		result = m_functions->EvaluateFeature(sl::kFeatureDLSS_RR, *m_functions->FrameToken,
			inputs, static_cast<uint32_t>(std::size(inputs)), commandList);
		if (result != sl::Result::eOk)
		{
			DebugConsole::LogError(std::string("Streamline: slEvaluateFeature failed: ")
				+ sl::getResultAsStr(result));
			return false;
		}

		return true;
#endif
	}

	void Streamline::FreeRayReconstructionResources()
	{
#ifdef UPDOWN_STREAMLINE
		if (m_initialized && m_optionsValid)
		{
			m_functions->FreeResources(sl::kFeatureDLSS_RR, kViewport);
		}
#endif
		m_optionsValid = false;
		m_configuredWidth = 0;
		m_configuredHeight = 0;
	}

	void Streamline::Shutdown()
	{
#ifdef UPDOWN_STREAMLINE
		if (m_initialized)
		{
			FreeRayReconstructionResources();
			m_functions->Shutdown();
			m_initialized = false;
		}

		if (m_interposer != nullptr)
		{
			::FreeLibrary(m_interposer);
			m_interposer = nullptr;
		}
#endif
		m_rayReconstructionSupported = false;
		m_optionsValid = false;
		m_functions.reset();
	}
}
