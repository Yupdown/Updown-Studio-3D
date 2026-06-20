#pragma once

#include "pch.h"

namespace udsdx
{
	using Vector2 = DirectX::SimpleMath::Vector2;
	using Vector2Int = XMINT2;
	using Vector3 = DirectX::SimpleMath::Vector3;
	using Vector3Int = XMINT3;
	using Vector4 = DirectX::SimpleMath::Vector4;
	using Vector4Int = XMINT4;
	using Color = DirectX::SimpleMath::Color;
	using Plane = DirectX::SimpleMath::Plane;
	using Quaternion = DirectX::SimpleMath::Quaternion;
	using Matrix4x4 = DirectX::SimpleMath::Matrix;

	struct Time
	{
		float deltaTime;
		float totalTime;
	};

	class Camera;
	class ShadowMap;
	class ScreenSpaceAO;
	class DeferredRenderer;
	class MotionBlur;
	class PostProcessBloom;
	class PostProcessTAA;
	class PostProcessOutline;
	class BoundingCamera;
	class EnvironmentMap;

	struct RenderOptions
	{
		bool DrawSSAO = true;
		bool DrawMotionBlur = true;
		bool DrawBloom = true;
		bool DrawTAA = true;
		bool DrawOutline = true;
		bool DrawShadowMap = true;
		unsigned int ShadowMapSize = 2048u;
		float MotionBlurShutterSpeed = 0.01f;
		Color FogColor = Color(1.381f, 1.691f, 2.000f, 1.0f);
		Color FogSunColor = Color(2.000f, 1.433f, 0.987f, 1.0f);
		float FogDensity = 0.03f;
		float FogHeightFalloff = 0.2f;
		float FogDistanceStart = 20.0f;
	};

	struct RenderParam
	{
		ID3D12Device* Device;
		ID3D12GraphicsCommandList* CommandList;
		ID3D12RootSignature* RootSignature;
		ID3D12DescriptorHeap* SRVDescriptorHeap;

		DeferredRenderer* Renderer;
		RenderOptions* RenderOptions;

		float AspectRatio;
		int FrameResourceIndex;
		int RenderStageIndex;
		const Time& Time;

		const D3D12_VIEWPORT& Viewport;
		const D3D12_RECT& ScissorRect;

		Camera* TargetCamera;
		BoundingCamera* ViewFrustumWorld;
		bool UseFrustumCulling;

		const D3D12_GPU_VIRTUAL_ADDRESS& ConstantBufferView;
		const D3D12_CPU_DESCRIPTOR_HANDLE& DepthStencilView;
		const D3D12_CPU_DESCRIPTOR_HANDLE& RenderTargetView;

		ID3D12Resource* RenderTargetResource;
		ID3D12Resource* DepthStencilResource;

		SpriteBatch* SpriteBatchNonPremultipliedAlpha;
		SpriteBatch* SpriteBatchPreMultipliedAlpha;

		ShadowMap* RenderShadowMap;
		ScreenSpaceAO* RenderScreenSpaceAO;
		MotionBlur* RenderMotionBlur;
		PostProcessBloom* RenderPostProcessBloom;
		PostProcessTAA* RenderPostProcessTAA;
		PostProcessOutline* RenderPostProcessOutline;
		EnvironmentMap* RenderEnvironmentMap;

		TracyD3D12Ctx* TracyQueueContext;
	};

	struct DescriptorParam
	{
		CD3DX12_CPU_DESCRIPTOR_HANDLE CbvCpuHandle;
		CD3DX12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle;
		CD3DX12_CPU_DESCRIPTOR_HANDLE RtvCpuHandle;
		CD3DX12_CPU_DESCRIPTOR_HANDLE DsvCpuHandle;

		CD3DX12_GPU_DESCRIPTOR_HANDLE CbvGpuHandle;
		CD3DX12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle;

		// GPU handle for the first descriptor of the SRV heap. Lets a Texture compute its own
		// absolute heap index from the current SrvGpuHandle, regardless of how many descriptors
		// other passes advanced past beforehand.
		CD3DX12_GPU_DESCRIPTOR_HANDLE SrvHeapStart;

		UINT CbvSrvUavDescriptorSize;
		UINT RtvDescriptorSize;
		UINT DsvDescriptorSize;
	};

	enum RootParam : UINT
	{
		PerObjectCBV,
		PerMaterialCBV,
		PerCameraCBV,
		BonesCBV,
		PrevBonesCBV,
		PerShadowCBV,
		PerFrameCBV,
		SrcTexTable,
	};

	enum RenderGroup : UINT
	{
		Forward = 0,
		Deferred = 1
	};

	static constexpr int FrameResourceCount = 2;
	static constexpr int SwapChainBufferCount = 2;
	static constexpr int NumTextureSlots = 16;

	// Sentinel for a texture without a registered SRV (no bindless heap index assigned).
	static constexpr UINT InvalidSrvIndex = 0xFFFFFFFFu;
}