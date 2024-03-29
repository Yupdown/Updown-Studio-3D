#pragma once

#include "pch.h"

namespace udsdx
{
	struct Time
	{
		float deltaTime;
		float totalTime;
	};

	struct RenderParam
	{
		ID3D12GraphicsCommandList* CommandList;
		ID3D12DescriptorHeap* SRVDescriptorHeap;
		float AspectRatio;
		UINT CbvSrvUavDescriptorSize;

		const D3D12_VIEWPORT& Viewport;
		const D3D12_RECT& ScissorRect;

		const D3D12_CPU_DESCRIPTOR_HANDLE& DepthStencilView;
		const D3D12_CPU_DESCRIPTOR_HANDLE& RenderTargetView;
	};

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
}