#pragma once

#include "pch.h"

namespace udsdx
{
	class ResourceObject
	{
	public:
		ResourceObject(std::string_view path);
		virtual ~ResourceObject();
	};
}