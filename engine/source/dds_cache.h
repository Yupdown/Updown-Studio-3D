#pragma once

#include "pch.h"

namespace udsdx
{
	// Owns the executable-side compressed-texture cache. Compressed textures are produced by the
	// external texconv tool (GPU BC7 for color, BC6H for HDR) and stored flat as "<hash>.dds" in a
	// single "ddscache" folder next to the executable. The file name is a deterministic 64-bit
	// FNV-1a hash of the source file's *contents*, so editing an image changes the name and forces a
	// recompress, while identical content reuses the existing entry - no on-disk mapping table is
	// required. Edited textures leave their previous content's DDS behind as an orphan; the folder
	// can be deleted wholesale to reclaim space.
	//
	// Accessed through INSTANCE(DDSCache). Resource loading is currently single-threaded, so this
	// type performs no locking.
	class DDSCache
	{
	public:
		DDSCache();
		~DDSCache();

		// Ensures a compressed DDS exists for the current contents of the given source texture and
		// returns its absolute path. Spawns texconv when no cache entry matches the source's content
		// hash. isHdr selects the compression format (BC6H_UF16 for HDR sources, BC7_UNORM
		// otherwise). Throws std::runtime_error when the source cannot be read, texconv.exe is
		// missing, or compression fails.
		std::filesystem::path GetCompressedTexture(std::wstring_view sourcePath, bool isHdr, TextureColorSpace colorSpace);

		// Same as above but for an in-memory compressed image (e.g. a texture embedded inside a
		// model file). 'data'/'size' point at the encoded bytes (PNG/JPG/etc.), 'formatHint' is the
		// image extension without a dot (e.g. L"png") used to name the temporary source texconv
		// reads. The cache name is the content hash of the bytes, so identical embedded and on-disk
		// images share a single cache entry. Throws std::runtime_error when texconv.exe is missing or
		// compression fails.
		std::filesystem::path GetCompressedTexture(const void* data, size_t size, std::wstring_view formatHint, bool isHdr, TextureColorSpace colorSpace);

	private:
		// Runs texconv to compress 'source' into 'ddsPath' (via a per-call temp directory named
		// after 'name' so concurrent compressions of different textures never collide).
		void RunTexconv(const std::filesystem::path& source, const std::filesystem::path& ddsPath, const std::wstring& name, bool isHdr, TextureColorSpace colorSpace);

	private:
		std::filesystem::path m_cacheDir;    // <exe dir>/ddscache
		std::filesystem::path m_texconvPath; // <exe dir>/texconv.exe
	};
}
