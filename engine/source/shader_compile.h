#pragma once

#include "pch.h"

namespace udsdx
{
	class ShaderIncludeHandler : public IDxcIncludeHandler
	{
	public:
		ShaderIncludeHandler(const std::wstring& shaderDirectory);

		HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR pFilename, IDxcBlob** ppIncludeSource) override;
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
		ULONG STDMETHODCALLTYPE AddRef() override;
		ULONG STDMETHODCALLTYPE Release() override;

	private:
		std::wstring m_shaderDirectory;
		ULONG m_refCount = 1;
	};

	ComPtr<IDxcBlob> CompileShader(const std::wstring& filename, const std::span<std::wstring>& defines, const std::wstring& entrypoint, const std::wstring& target);
	ComPtr<IDxcBlob> CompileShaderFromMemory(const std::string& data, const std::span<std::wstring>& defines, const std::wstring& entrypoint, const std::wstring& target);

	// Returns whether the shader, once preprocessed with the given defines, defines the named entry
	// point. Lets callers skip compiling absent optional stages (GS/HS/DS) without provoking a
	// "entry point not found" compile error. Honors #ifdef and #include because it preprocesses.
	bool ShaderHasEntryPoint(const std::wstring& filename, const std::span<std::wstring>& defines, const std::wstring& entrypoint);
}