#pragma once

#include "pch.h"

namespace udsdx
{
	struct ObjectConstants
	{
		XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
	};

	class FrameResource
	{
	private:
		ComPtr<ID3D12CommandAllocator> m_commandListAllocator;
		std::unique_ptr<UploadBuffer<ObjectConstants>> m_objectCB = nullptr;

		UINT64 m_fence = 0;

	public:
		FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount);
		FrameResource(const FrameResource& rhs) = delete;
		FrameResource& operator=(const FrameResource& rhs) = delete;
		~FrameResource();

	public:
		ID3D12CommandAllocator* GetCommandListAllocator() const;
		UploadBuffer<ObjectConstants>* GetObjectCB() const;

		UINT64 GetFence() const;
		void SetFence(UINT64 fence);
	};
}