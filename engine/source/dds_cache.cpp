#include "pch.h"
#include "dds_cache.h"
#include "resource_load.h"
#include "debug_console.h"

namespace udsdx
{
	namespace
	{
		std::string WideToUtf8(const std::wstring& wide)
		{
			if (wide.empty())
			{
				return {};
			}
			int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
			std::string result(static_cast<size_t>(size), '\0');
			::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
			return result;
		}

		// Deterministic 64-bit FNV-1a over the UTF-8 bytes of the key, rendered as 16 lowercase hex
		// digits. A fixed algorithm (not std::hash, whose values are not guaranteed stable across
		// runs or toolchains) so the cache file name survives rebuilds and compiler updates.
		std::wstring HashName(const std::wstring& key)
		{
			const std::string utf8 = WideToUtf8(key);
			uint64_t hash = 0xcbf29ce484222325ULL; // FNV-1a 64-bit offset basis
			for (unsigned char byte : utf8)
			{
				hash ^= static_cast<uint64_t>(byte);
				hash *= 0x100000001b3ULL; // FNV-1a 64-bit prime
			}

			static const wchar_t* const digits = L"0123456789abcdef";
			std::wstring name(16, L'0');
			for (int i = 15; i >= 0; --i)
			{
				name[i] = digits[hash & 0xFULL];
				hash >>= 4;
			}
			return name;
		}
	}

	DDSCache::DDSCache()
	{
		std::filesystem::path executableDirectory = Resource::GetExecutableDirectory();
		m_cacheDir = executableDirectory / L"ddscache";
		m_texconvPath = executableDirectory / L"texconv.exe";

		std::error_code ec;
		std::filesystem::create_directories(m_cacheDir, ec);
	}

	DDSCache::~DDSCache()
	{
	}

	std::filesystem::path DDSCache::GetCompressedTexture(std::wstring_view sourcePath, bool isHdr)
	{
		if (!std::filesystem::exists(m_texconvPath))
		{
			throw std::runtime_error("texconv.exe was not found next to the executable. Rebuild the demo target so the post-build step copies it into the executable directory.");
		}

		// The cache file name is derived purely from the normalized source path, so it always
		// resolves to the same file without consulting any persisted mapping.
		std::wstring name = HashName(Resource::NormalizePath(sourcePath));
		std::filesystem::path ddsPath = m_cacheDir / (name + L".dds");

		std::filesystem::path source(sourcePath);

		// Reuse the cache only when it is at least as new as the source. The compressed file's
		// modification time is stamped to match the source after compression (below) so staleness
		// can be detected purely from timestamps.
		bool upToDate = false;
		std::error_code ec;
		if (std::filesystem::exists(ddsPath, ec) && std::filesystem::exists(source, ec))
		{
			auto sourceTime = std::filesystem::last_write_time(source, ec);
			auto ddsTime = std::filesystem::last_write_time(ddsPath, ec);
			if (!ec && ddsTime >= sourceTime)
			{
				upToDate = true;
			}
		}

		if (!upToDate)
		{
			RunTexconv(source, ddsPath, name, isHdr);
			std::filesystem::last_write_time(ddsPath, std::filesystem::last_write_time(source, ec), ec);
			DebugConsole::Log("\tTexture compressed and cached: " + ddsPath.string());
		}

		return ddsPath;
	}

	void DDSCache::RunTexconv(const std::filesystem::path& source, const std::filesystem::path& ddsPath, const std::wstring& name, bool isHdr)
	{
		std::filesystem::path sourceAbsolute = std::filesystem::absolute(source);
		std::filesystem::path tempDir = m_cacheDir / (name + L".tmp");

		std::error_code ec;
		std::filesystem::remove_all(tempDir, ec);
		std::filesystem::create_directories(tempDir, ec);

		// texconv only accelerates BC7/BC6H on the GPU; BC1-BC5 stay on the CPU. HDR sources keep
		// the HDR-friendly BC6H format, everything else uses BC7.
		const wchar_t* format = isHdr ? L"BC6H_UF16" : L"BC7_UNORM";

		// texconv.exe -nologo -y -m 0 -f <format> -gpu 0 -o "<tempDir>" "<source>"
		std::wstring commandLine;
		commandLine += L"\"" + m_texconvPath.wstring() + L"\"";
		commandLine += L" -nologo -y -m 0 -f ";
		commandLine += format;
		commandLine += L" -gpu 0";
		commandLine += L" -o \"" + tempDir.wstring() + L"\"";
		commandLine += L" \"" + sourceAbsolute.wstring() + L"\"";

		STARTUPINFOW startupInfo = {};
		startupInfo.cb = sizeof(startupInfo);
		PROCESS_INFORMATION processInfo = {};

		// CreateProcessW may write to the command-line buffer, so it must be mutable.
		std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
		mutableCommandLine.push_back(L'\0');

		BOOL created = ::CreateProcessW(
			m_texconvPath.c_str(),
			mutableCommandLine.data(),
			nullptr, nullptr, FALSE,
			CREATE_NO_WINDOW,
			nullptr, nullptr,
			&startupInfo, &processInfo);

		if (!created)
		{
			DWORD error = ::GetLastError();
			std::filesystem::remove_all(tempDir, ec);
			throw std::runtime_error("Failed to launch texconv.exe (CreateProcessW error " + std::to_string(error) + ").");
		}

		::WaitForSingleObject(processInfo.hProcess, INFINITE);

		DWORD exitCode = 1;
		::GetExitCodeProcess(processInfo.hProcess, &exitCode);
		::CloseHandle(processInfo.hProcess);
		::CloseHandle(processInfo.hThread);

		if (exitCode != 0)
		{
			std::filesystem::remove_all(tempDir, ec);
			throw std::runtime_error("texconv failed (exit code " + std::to_string(exitCode) + ") for: " + sourceAbsolute.string());
		}

		// texconv names its output "<sourceStem>.DDS" inside the output directory.
		std::filesystem::path produced = tempDir / (sourceAbsolute.stem().wstring() + L".DDS");
		if (!std::filesystem::exists(produced))
		{
			// Defensive: pick up whatever single file texconv emitted, regardless of extension casing.
			for (const auto& entry : std::filesystem::directory_iterator(tempDir, ec))
			{
				if (entry.is_regular_file())
				{
					produced = entry.path();
					break;
				}
			}
		}

		std::filesystem::remove(ddsPath, ec);
		std::filesystem::rename(produced, ddsPath, ec);
		if (ec)
		{
			// rename can fail (e.g. across volumes); fall back to copy + cleanup.
			std::error_code copyEc;
			std::filesystem::copy_file(produced, ddsPath, std::filesystem::copy_options::overwrite_existing, copyEc);
		}
		std::filesystem::remove_all(tempDir, ec);

		if (!std::filesystem::exists(ddsPath))
		{
			throw std::runtime_error("texconv did not produce an output file for: " + sourceAbsolute.string());
		}
	}
}
