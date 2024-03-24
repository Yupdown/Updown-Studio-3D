#include "pch.h"
#include "vertex.h"

namespace udsdx
{
	Vertex::Vertex()
	{
		position = { 0.0f, 0.0f, 0.0f };
		color = { 1.0f, 1.0f, 1.0f, 1.0f };
		uv = { 0.0f, 0.0f };
		normal = { 0.0f, 0.0f, 0.0f };
	}

	Vertex::Vertex(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT4& color, const DirectX::XMFLOAT2& uv, const DirectX::XMFLOAT3& normal)
	{
		this->position = position;
		this->color = color;
		this->uv = uv;
		this->normal = normal;
	}
}