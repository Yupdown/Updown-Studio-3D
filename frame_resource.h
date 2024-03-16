#pragma once

#include "pch.h"

namespace udsdx
{
	struct PassConstants
	{
		XMFLOAT4X4 ViewMatrix = MathHelper::Identity4x4();
		XMFLOAT4X4 ProjMatrix = MathHelper::Identity4x4();
		XMFLOAT4 CameraPosition = { 0.0f, 0.0f, 0.0f, 1.0f };
		float TotalTime = 0.0f;
	};

	class FrameResource
	{
	public:
		FrameResource(ID3D12Device* device);
		FrameResource(const FrameResource& rhs) = delete;
		FrameResource& operator=(const FrameResource& rhs) = delete;
		~FrameResource();

	public:
		ID3D12CommandAllocator* GetCommandListAllocator() const;
		UploadBuffer<PassConstants>* GetObjectCB() const;

		UINT64 GetFence() const;
		void SetFence(UINT64 fence);

	private:
		ComPtr<ID3D12CommandAllocator> m_commandListAllocator;
		std::unique_ptr<UploadBuffer<PassConstants>> m_objectCB = nullptr;

		UINT64 m_fence = 0;
	};
}