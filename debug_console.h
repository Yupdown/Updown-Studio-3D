#pragma once

#include "pch.h"

namespace udsdx
{
	class DebugConsole
	{
	private:
		static constexpr int STACKTRACE_SIZE = 16;

	public:
		static void Log(const std::string& message);
		static void LogWarning(const std::string& message);
		static void LogError(const std::string& message);
	};
}