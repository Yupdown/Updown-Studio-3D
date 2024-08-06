#pragma once

#include "pch.h"
#include "resource_object.h"
#include "mesh.h"

namespace udsdx
{
	struct Animation
	{
		struct Channel
		{
			std::string Name{};

			std::vector<float> PositionTimestamps{};
			std::vector<float> RotationTimestamps{};
			std::vector<float> ScaleTimestamps{};

			std::vector<Vector3> Positions{};
			std::vector<Quaternion> Rotations{};
			std::vector<Vector3> Scales{};
		};

		std::string Name{};

		float Duration = 0.0f;
		float TicksPerSecond = 1.0f;

		std::vector<Channel> Channels{};
	};

	struct Bone
	{
		std::string Name{};
		Matrix4x4 Offset{};
		Matrix4x4 Transform{};
	};

	class RiggedMesh : public ResourceObject
	{
	public:
		RiggedMesh(std::wstring_view resourcePath);

		void PopulateTransforms(std::string_view animationKey, float time, std::vector<Matrix4x4>& out) const;

		void CreateBuffers(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);
		D3D12_VERTEX_BUFFER_VIEW VertexBufferView() const;
		D3D12_INDEX_BUFFER_VIEW IndexBufferView() const;
		const std::vector<Submesh>& GetSubmeshes() const;

		// We can free this memory after we finish upload to the GPU.
		void DisposeUploaders();

		const BoundingBox& GetBounds() const;

	protected:
		// System memory copies.  Use Blobs because the vertex/index format can be generic.
		// It is up to the client to cast appropriately.  
		ComPtr<ID3DBlob> m_vertexBufferCPU = nullptr;
		ComPtr<ID3DBlob> m_indexBufferCPU = nullptr;

		ComPtr<ID3D12Resource> m_vertexBufferGPU = nullptr;
		ComPtr<ID3D12Resource> m_indexBufferGPU = nullptr;

		ComPtr<ID3D12Resource> m_vertexBufferUploader = nullptr;
		ComPtr<ID3D12Resource> m_indexBufferUploader = nullptr;

		std::vector<Submesh> m_submeshes;

		// Data about the buffers.
		UINT m_vertexByteStride = 0;
		UINT m_vertexBufferByteSize = 0;
		DXGI_FORMAT m_indexFormat = DXGI_FORMAT_R32_UINT;
		UINT m_indexBufferByteSize = 0;

		BoundingBox m_bounds;

		std::vector<Bone> m_bones;
		std::vector<int> m_boneParents;

		std::unordered_map<std::string, Animation> m_animations;
	};
}