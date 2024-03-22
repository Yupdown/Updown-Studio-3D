#include "pch.h"
#include "debug_console.h"

namespace udsdx
{
	void DebugConsole::Log(const std::string& message)
	{
		TracyMessageCS(message.c_str(), message.size(), 0xFFFFFF, STACKTRACE_SIZE);
		std::cout << "[LOG] " << message << std::endl;
	}

	void DebugConsole::LogWarning(const std::string& message)
	{
		TracyMessageCS(message.c_str(), message.size(), 0xFFFF00, STACKTRACE_SIZE);
		std::cout << "[WARNING] " << message << std::endl;
	}

	void DebugConsole::LogError(const std::string& message)
	{
		TracyMessageCS(message.c_str(), message.size(), 0xFF0000, STACKTRACE_SIZE);
		std::cout << "[ERROR] " << message << std::endl;
	}
}
