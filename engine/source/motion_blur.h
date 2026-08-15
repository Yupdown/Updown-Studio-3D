#pragma once

#include "pch.h"

namespace udsdx
{
	class Camera;

	class MotionBlur
	{
	public:
		constexpr const static UINT MaxBlurRadius = 20;

	public:
		MotionBlur(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList);
		MotionBlur(const MotionBlur& rhs) = delete;
		MotionBlur& operator=(const MotionBlur& rhs) = delete;
		~MotionBlur() = default;

		// The velocity and depth sources are supplied by the caller rather than read from the
		// G-buffer: the raytracing path produces its own motion vectors and has no depth buffer at
		// all. Those buffers are not necessarily the size of the screen either -- the raytracer
		// traces into smaller ones and lets the upscaler make up the difference -- so the velocity
		// extent comes with them. The pixel shader reads everything by UV and does not care, but
		// the tile pass indexes texels directly and has to be told. depthIsLinear selects whether
		// the depth SRV holds reverse-Z NDC that still needs linearizing, or an already-linear
		// depth.
		void Pass(RenderParam& param, D3D12_GPU_VIRTUAL_ADDRESS cbvGpu,
			D3D12_GPU_DESCRIPTOR_HANDLE velocitySrv,
			UINT velocityWidth, UINT velocityHeight,
			D3D12_GPU_DESCRIPTOR_HANDLE depthSrv,
			bool depthIsLinear);

		void OnResize(UINT newWidth, UINT newHeight);
		void BuildResources();
		void BuildDescriptors(DescriptorParam& descriptorParam);
		void RebuildDescriptors();
		void BuildRootSignature();
		void BuildPipelineState();

	private:
		ID3D12Device* m_device;

		UINT m_width = 0;
		UINT m_height = 0;

		CD3DX12_CPU_DESCRIPTOR_HANDLE m_tileMaxCpuSrv;
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_tileMaxGpuSrv;
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_tileMaxCpuUav;
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_tileMaxGpuUav;

		CD3DX12_CPU_DESCRIPTOR_HANDLE m_neighborMaxCpuSrv;
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_neighborMaxGpuSrv;
		CD3DX12_CPU_DESCRIPTOR_HANDLE m_neighborMaxCpuUav;
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_neighborMaxGpuUav;

		CD3DX12_CPU_DESCRIPTOR_HANDLE m_sourceCpuSrv;
		CD3DX12_GPU_DESCRIPTOR_HANDLE m_sourceGpuSrv;

		ComPtr<ID3D12Resource> m_tileMaxBuffer;
		ComPtr<ID3D12Resource> m_neighborMaxBuffer;
		ComPtr<ID3D12Resource> m_sourceBuffer;

		ComPtr<ID3D12RootSignature> m_computeRootSignature;
		ComPtr<ID3D12RootSignature> m_rootSignature;

		ComPtr<ID3D12PipelineState> m_tileMaxPso;
		ComPtr<ID3D12PipelineState> m_neighborMaxPso;
		ComPtr<ID3D12PipelineState> m_pso;
	};
}