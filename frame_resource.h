#pragma once

#include "pch.h"

namespace udsdx
{
	struct ObjectConstants
	{
		Matrix4x4 World = Matrix4x4::Identity;
	};

	struct CameraConstants
	{
		Matrix4x4 ViewProj = Matrix4x4::Identity;
		Vector4 CameraPosition = Vector4::Zero;
	};

	struct PassConstants
	{
		Matrix4x4 ShadowTransform = Matrix4x4::Identity;
		Vector3 LightDirection = Vector3::Zero;
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