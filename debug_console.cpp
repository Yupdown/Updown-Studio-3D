#include "pch.h"
#include "debug_console.h"

namespace udsdx
{
	std::string ToNarrowString(std::wstring_view wideString)
	{
		std::string narrowString;
		narrowString.resize(wideString.size());
		wcstombs_s(nullptr, narrowString.data(), narrowString.size() + 1, wideString.data(), wideString.size());
		return narrowString;
	}

	void DebugConsole::Log(std::string_view message)
	{
		TracyMessageCS(message.data(), message.size(), 0xFFFFFF, STACKTRACE_SIZE);
		std::cout << "[LOG] " << message << std::endl;
		OutputDebugStringA(message.data());
	}

	void DebugConsole::Log(std::wstring_view message)
	{
		TracyMessageCS(ToNarrowString(message).c_str(), message.size(), 0xFFFFFF, STACKTRACE_SIZE);
		std::wcout << L"[LOG] " << message << std::endl;
		OutputDebugStringW(message.data());
	}

	void DebugConsole::LogWarning(std::string_view message)
	{
		TracyMessageCS(message.data(), message.size(), 0xFFFF00, STACKTRACE_SIZE);
		std::cout << "[WARNING] " << message << std::endl;
		OutputDebugStringA(message.data());
	}

	void DebugConsole::LogWarning(std::wstring_view message)
	{
		TracyMessageCS(ToNarrowString(message).c_str(), message.size(), 0xFFFF00, STACKTRACE_SIZE);
		std::wcout << L"[WARNING] " << message << std::endl;
		OutputDebugStringW(message.data());
	}

	void DebugConsole::LogError(std::string_view message)
	{
		TracyMessageCS(message.data(), message.size(), 0xFF0000, STACKTRACE_SIZE);
		std::cout << "[ERROR] " << message << std::endl;
		OutputDebugStringA(message.data());
	}

	void DebugConsole::LogError(std::wstring_view message)
	{
		TracyMessageCS(ToNarrowString(message).c_str(), message.size(), 0xFF0000, STACKTRACE_SIZE);
		std::wcout << L"[ERROR] " << message << std::endl;
		OutputDebugStringW(message.data());
	}
}
