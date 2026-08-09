#pragma once

#include "pch.h"
#include "define.h"

namespace udsdx
{
	// Everything DLSS Ray Reconstruction needs for one frame, in engine types. Keeping the SL
	// structures out of this header is deliberate: sl.h pulls in the whole SDK, and Core includes
	// this file.
	//
	// Matrices are row-major with the row-vector convention -- the engine's own, and Streamline's
	// too. They must NOT be transposed the way the HLSL constant buffer upload transposes them,
	// and they must not carry the jitter offset, which travels separately.
	struct RayReconstructionFrame
	{
		ID3D12Resource* NoisyColor = nullptr;
		ID3D12Resource* Output = nullptr;
		ID3D12Resource* LinearDepth = nullptr;
		ID3D12Resource* MotionVectors = nullptr;
		ID3D12Resource* Albedo = nullptr;
		ID3D12Resource* SpecularAlbedo = nullptr;
		ID3D12Resource* NormalRoughness = nullptr;

		// Manual hooking means SL cannot track resource states, so the host has to state them.
		D3D12_RESOURCE_STATES InputState = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES OutputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

		UINT Width = 0;
		UINT Height = 0;

		Matrix4x4 ViewToClip = Matrix4x4::Identity;
		Matrix4x4 ClipToView = Matrix4x4::Identity;
		Matrix4x4 ClipToPrevClip = Matrix4x4::Identity;
		Matrix4x4 PrevClipToClip = Matrix4x4::Identity;

		Vector2 JitterOffset{};
		Vector3 CameraPosition{};
		Vector3 CameraRight{};
		Vector3 CameraUp{};
		Vector3 CameraForward{};

		float CameraNear = 0.0f;
		float CameraFar = 0.0f;
		float CameraFovY = 0.0f;
		float CameraAspectRatio = 1.0f;

		// Tells the denoiser that nothing from the previous frame can be reused.
		bool ResetHistory = false;
	};

	// Runtime loader for the NVIDIA Streamline SDK, used to reach DLSS Ray Reconstruction.
	//
	// Streamline offers two integration modes. The usual one replaces d3d12.lib/dxgi.lib with
	// sl.interposer.lib so SL silently intercepts device and swap chain creation. This engine uses
	// the other one -- manual hooking -- because Ray Reconstruction only exists on NVIDIA RTX
	// hardware while the raytracer itself runs on any DXR adapter. Linking the interposer would
	// make sl.interposer.dll a hard load-time dependency of the executable on every machine;
	// loading it here instead means a missing DLL, a non-NVIDIA adapter or an out-of-date driver
	// simply leaves IsAvailable() false and the engine keeps its own denoiser.
	//
	// The cost of that choice is UpgradeInterface: SL still has to see a handful of DXGI and D3D12
	// calls, so the host hands it those objects and uses the proxies it gets back for exactly those
	// calls. See ProgrammingGuideManualHooking.md section 2 for the mandatory list.
	class Streamline
	{
	public:
		// Both defined in the translation unit, not defaulted here: the unique_ptr member below
		// points at an incomplete type, and an in-class definition would force every construction
		// site to instantiate its destructor for the unwinding path.
		Streamline();
		Streamline(const Streamline&) = delete;
		Streamline& operator=(const Streamline&) = delete;
		~Streamline();

		// Loads sl.interposer.dll and calls slInit. Safe to call on any machine: every failure
		// path logs a reason and leaves the object inert. Must run before the swap chain exists,
		// because SL hooks its creation.
		void Initialize();

		// Hands SL the device and resolves which features this adapter can actually run. The
		// adapter LUID is what slIsFeatureSupported matches against, so the device has to exist
		// before the answer is meaningful.
		void OnDeviceCreated(ID3D12Device* device, IDXGIAdapter1* adapter);

		void Shutdown();

		// Wraps a DXGI/D3D12 interface in an SL proxy, in place. Returns false and leaves the
		// pointer untouched when Streamline is not running, which is what keeps every call site a
		// plain "use whatever this returns" rather than a branch on availability.
		//
		// Only the calls SL actually hooks may go through the proxy. Everything else -- and every
		// third-party library -- must keep seeing the native object.
		bool UpgradeInterface(void** iface);

		// Starts a frame and mints the token every other per-frame call has to match. Cheap enough
		// to call unconditionally; does nothing when SL is unavailable.
		void BeginFrame(UINT frameIndex);

		// Configures Ray Reconstruction for this output size. Re-applying identical options is a
		// no-op inside SL, so this can be called every frame.
		bool SetRayReconstructionOptions(UINT width, UINT height);

		// Tags the inputs, uploads the camera constants and runs the denoiser into frame.Output.
		// The caller owns restoring command list state afterwards.
		bool EvaluateRayReconstruction(ID3D12GraphicsCommandList* commandList, const RayReconstructionFrame& frame);

		// Releases the denoiser's internal allocations. Needed on resize and when leaving the mode,
		// otherwise SL keeps a viewport's worth of VRAM per size it has ever seen.
		void FreeRayReconstructionResources();

		// True once slInit has succeeded; says nothing about any individual feature.
		bool IsAvailable() const { return m_initialized; }
		bool IsRayReconstructionSupported() const { return m_rayReconstructionSupported; }
		// Human-readable reason the feature is unavailable, for the debug panel. Empty when it is.
		const std::string& GetStatusMessage() const { return m_statusMessage; }

	private:
		// Resolved entry points, defined in the translation unit so the SL headers stay out of
		// every file that sees Core.
		struct Functions;

		HMODULE m_interposer = nullptr;
		std::unique_ptr<Functions> m_functions;
		bool m_initialized = false;
		bool m_rayReconstructionSupported = false;
		bool m_optionsValid = false;
		UINT m_configuredWidth = 0;
		UINT m_configuredHeight = 0;
		std::string m_statusMessage = "not built with Streamline support";
	};
}
