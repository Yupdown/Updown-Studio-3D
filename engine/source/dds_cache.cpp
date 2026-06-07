#include "pch.h"
#include "dds_cache.h"
#include "resource_load.h"
#include "debug_console.h"

namespace udsdx
{
	namespace
	{
		// Renders a 64-bit FNV-1a accumulator as 16 lowercase hex digits.
		std::wstring HashToHex(uint64_t hash)
		{
			static const wchar_t* const digits = L"0123456789abcdef";
			std::wstring name(16, L'0');
			for (int i = 15; i >= 0; --i)
			{
				name[i] = digits[hash & 0xFULL];
				hash >>= 4;
			}
			return name;
		}

		// Deterministic 64-bit FNV-1a over the raw bytes of a file, rendered as 16 lowercase hex
		// digits. Returns an empty string if the file cannot be opened. A fixed algorithm (not
		// std::hash, whose values are not guaranteed stable across runs or toolchains) so the cache
		// file name survives rebuilds and compiler updates.
		std::wstring HashFileContent(const std::filesystem::path& path)
		{
			std::ifstream file(path, std::ios::binary);
			if (!file.is_open())
			{
				return {};
			}

			uint64_t hash = 0xcbf29ce484222325ULL; // FNV-1a 64-bit offset basis
			char buffer[64 * 1024];
			while (file)
			{
				file.read(buffer, sizeof(buffer));
				const std::streamsize count = file.gcount();
				for (std::streamsize i = 0; i < count; ++i)
				{
					hash ^= static_cast<uint64_t>(static_cast<unsigned char>(buffer[i]));
					hash *= 0x100000001b3ULL; // FNV-1a 64-bit prime
				}
			}

			return HashToHex(hash);
		}

		// Same FNV-1a as HashFileContent, but over an in-memory buffer, so an embedded image and an
		// identical loose file produce the same cache name.
		std::wstring HashMemory(const void* data, size_t size)
		{
			uint64_t hash = 0xcbf29ce484222325ULL; // FNV-1a 64-bit offset basis
			const unsigned char* bytes = static_cast<const unsigned char*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				hash ^= static_cast<uint64_t>(bytes[i]);
				hash *= 0x100000001b3ULL; // FNV-1a 64-bit prime
			}

			return HashToHex(hash);
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

		std::filesystem::path source(sourcePath);

		// The cache file name is the hash of the source's *contents*. Any edit to the image yields a
		// different name and forces a recompress, while identical content (even restored from an
		// older copy with an older timestamp) reuses the existing entry. The previous content's DDS
		// is left behind as an orphan; delete the ddscache folder to reclaim space.
		std::wstring name = HashFileContent(source);
		if (name.empty())
		{
			throw std::runtime_error("Failed to read source texture for hashing: " + source.string());
		}
		std::filesystem::path ddsPath = m_cacheDir / (name + L".dds");

		if (!std::filesystem::exists(ddsPath))
		{
			RunTexconv(source, ddsPath, name, isHdr);
			DebugConsole::Log("\tTexture compressed and cached: " + ddsPath.string());
		}

		return ddsPath;
	}

	std::filesystem::path DDSCache::GetCompressedTexture(const void* data, size_t size, std::wstring_view formatHint, bool isHdr)
	{
		if (!std::filesystem::exists(m_texconvPath))
		{
			throw std::runtime_error("texconv.exe was not found next to the executable. Rebuild the demo target so the post-build step copies it into the executable directory.");
		}

		if (data == nullptr || size == 0)
		{
			throw std::runtime_error("Embedded texture has no data to compress.");
		}

		std::wstring name = HashMemory(data, size);
		std::filesystem::path ddsPath = m_cacheDir / (name + L".dds");

		if (!std::filesystem::exists(ddsPath))
		{
			// texconv only reads files, so stage the embedded bytes to a temporary source whose
			// extension matches the encoded format, then run the identical compression path.
			std::wstring extension = formatHint.empty() ? L"png" : std::wstring(formatHint);
			std::filesystem::path tempSource = m_cacheDir / (name + L"." + extension);

			std::error_code ec;
			{
				std::ofstream out(tempSource, std::ios::binary | std::ios::trunc);
				if (!out.is_open())
				{
					throw std::runtime_error("Failed to stage embedded texture for compression: " + tempSource.string());
				}
				out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
			}

			try
			{
				RunTexconv(tempSource, ddsPath, name, isHdr);
			}
			catch (...)
			{
				std::filesystem::remove(tempSource, ec);
				throw;
			}
			std::filesystem::remove(tempSource, ec);
			DebugConsole::Log("\tEmbedded texture compressed and cached: " + ddsPath.string());
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
