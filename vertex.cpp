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
}