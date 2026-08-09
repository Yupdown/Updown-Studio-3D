#pragma once

#include "pch.h"

namespace udsdx
{
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
	// Ray Reconstruction also does not need swap chain interposition (unlike frame generation), so
	// the manual-hooking surface stays small: initialise, hand SL the device, ask what it supports.
	class Streamline
	{
	public:
		Streamline() = default;
		Streamline(const Streamline&) = delete;
		Streamline& operator=(const Streamline&) = delete;
		~Streamline();

		// Loads sl.interposer.dll and calls slInit. Safe to call on any machine: every failure
		// path logs a reason and leaves the object inert. Must run before device creation, which
		// is what slInit documents and what later phases will need once device creation is routed
		// through the interposer's own D3D12CreateDevice.
		void Initialize();

		// Hands SL the device and resolves which features this adapter can actually run. The
		// adapter LUID is what slIsFeatureSupported matches against, so the device has to exist
		// before the answer is meaningful.
		void OnDeviceCreated(ID3D12Device* device, IDXGIAdapter1* adapter);

		void Shutdown();

		// True once slInit has succeeded; says nothing about any individual feature.
		bool IsAvailable() const { return m_initialized; }
		bool IsRayReconstructionSupported() const { return m_rayReconstructionSupported; }
		// Human-readable reason the feature is unavailable, for the debug panel. Empty when it is.
		const std::string& GetStatusMessage() const { return m_statusMessage; }

	private:
		HMODULE m_interposer = nullptr;
		bool m_initialized = false;
		bool m_rayReconstructionSupported = false;
		std::string m_statusMessage = "not built with Streamline support";

		// Resolved from sl.interposer.dll. Kept as void* so the header does not have to include
		// the SL headers, which would leak them into every translation unit that sees Core.
		void* m_slShutdown = nullptr;
		void* m_slIsFeatureSupported = nullptr;
		void* m_slSetD3DDevice = nullptr;
	};
}
