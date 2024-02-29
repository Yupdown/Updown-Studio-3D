#include "pch.h"
#include "frame_resource.h"

namespace udsdx
{
	FrameResource::FrameResource(ID3D12Device* device, UINT passCount, UINT objectCount)
	{
		ThrowIfFailed(device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(&m_commandListAllocator)
		));
		m_objectCB = std::make_unique<UploadBuffer<ObjectConstants>>(device, objectCount, true);
	}

	FrameResource::~FrameResource()
	{

	}

	ID3D12CommandAllocator* FrameResource::GetCommandListAllocator() const
	{
		return m_commandListAllocator.Get();
	}

	UploadBuffer<ObjectConstants>* FrameResource::GetObjectCB() const
	{
		return m_objectCB.get();
	}

	UINT64 FrameResource::GetFence() const
	{
		return m_fence;
	}

	void FrameResource::SetFence(UINT64 fence)
	{
		m_fence = fence;
	}
}