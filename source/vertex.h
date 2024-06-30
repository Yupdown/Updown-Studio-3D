#pragma once

#include "pch.h"

namespace udsdx
{
	struct Vertex
	{
		XMFLOAT3 position;
		XMFLOAT4 color;
		XMFLOAT2 uv;
		XMFLOAT3 normal;

		// Input Layout:
		// used to make vertex propertices associate with the semantics of shaders
		constexpr static D3D12_INPUT_ELEMENT_DESC DescriptionTable[] = {
			// Vertex::position
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			// Vertex::color
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			// Vertex::texcoord
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			// Vertex::normal
			{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};
		constexpr static UINT DescriptionTableSize = sizeof(DescriptionTable) / sizeof(D3D12_INPUT_ELEMENT_DESC);

		Vertex();
		Vertex(const XMFLOAT3& position, const XMFLOAT4& color, const XMFLOAT2& uv, const XMFLOAT3& normal);
	};
}