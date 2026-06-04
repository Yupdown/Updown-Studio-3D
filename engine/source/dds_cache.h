#pragma once

#include "pch.h"

namespace udsdx
{
	// Owns the executable-side compressed-texture cache. Compressed textures are produced by the
	// external texconv tool (GPU BC7 for color, BC6H for HDR) and stored flat as "<hash>.dds" in a
	// single "ddscache" folder next to the executable. The file name is a deterministic 64-bit
	// FNV-1a hash of the normalized source path, so the cache file for any source can be located by
	// recomputing the hash - no on-disk mapping table is required.
	//
	// Accessed through INSTANCE(DDSCache). Resource loading is currently single-threaded, so this
	// type performs no locking.
	class DDSCache
	{
	public:
		DDSCache();
		~DDSCache();

		// Ensures an up-to-date compressed DDS exists for the given source texture and returns its
		// absolute path. Spawns texconv when the cache is missing or stale. isHdr selects the
		// compression format (BC6H_UF16 for HDR sources, BC7_UNORM otherwise). Throws
		// std::runtime_error when texconv.exe is missing or compression fails.
		std::filesystem::path GetCompressedTexture(std::wstring_view sourcePath, bool isHdr);

	private:
		// Runs texconv to compress 'source' into 'ddsPath' (via a per-call temp directory named
		// after 'name' so concurrent compressions of different textures never collide).
		void RunTexconv(const std::filesystem::path& source, const std::filesystem::path& ddsPath, const std::wstring& name, bool isHdr);

	private:
		std::filesystem::path m_cacheDir;    // <exe dir>/ddscache
		std::filesystem::path m_texconvPath; // <exe dir>/texconv.exe
	};
}
