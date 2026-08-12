#include "pch.h"
#include "shader_compile.h"
#include "debug_console.h"
#include "embedded_shaders/inc_common_hlsl.h"
#include "embedded_shaders/inc_brdf_hlsl.h"

namespace udsdx
{
    static ComPtr<IDxcUtils> g_pUtils;
    static ComPtr<IDxcCompiler3> g_pCompiler;

    namespace
    {
        // Shaders under resource/shader are compiled at runtime from wherever the app keeps them,
        // with no include path, so their headers cannot be found on disk. They are baked into the
        // executable instead and served from here.
        struct EmbeddedHeader
        {
            const wchar_t* Name;
            const unsigned char* Data;
            unsigned int Size;
        };

        constexpr EmbeddedHeader kEmbeddedHeaders[] = {
            { L"inc_common.hlsl", g_embedded_common_hlsl, g_embedded_common_hlsl_size },
            { L"inc_brdf.hlsl",   g_embedded_brdf_hlsl,   g_embedded_brdf_hlsl_size   },
        };

        // DXC hands quoted includes over as "./name.hlsl"; match on the bare file name so both
        // spellings resolve.
        std::wstring_view StripRelativePrefix(const wchar_t* filename)
        {
            std::wstring_view name(filename);
            while (name.starts_with(L"./") || name.starts_with(L".\\"))
            {
                name.remove_prefix(2);
            }
            return name;
        }
    }

    // Scans preprocessed HLSL for a definition/declaration of `name`, i.e. an occurrence of the
    // identifier on a token boundary immediately followed (modulo whitespace) by '('. Good enough
    // to detect entry-point functions, whose names are unique tokens in our shaders.
    static bool SourceDefinesFunction(std::string_view src, std::string_view name)
    {
        auto isWord = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        };
        for (size_t pos = 0; (pos = src.find(name, pos)) != std::string_view::npos; pos += name.size())
        {
            if (pos > 0 && isWord(src[pos - 1]))
            {
                continue; // part of a longer identifier
            }
            size_t j = pos + name.size();
            while (j < src.size() && (src[j] == ' ' || src[j] == '\t' || src[j] == '\r' || src[j] == '\n'))
            {
                ++j;
            }
            if (j < src.size() && src[j] == '(')
            {
                return true;
            }
        }
        return false;
    }

    ShaderIncludeHandler::ShaderIncludeHandler(const std::wstring& shaderDirectory) : m_shaderDirectory(shaderDirectory)
    {
    }

    HRESULT STDMETHODCALLTYPE ShaderIncludeHandler::LoadSource(LPCWSTR pFilename, IDxcBlob** ppIncludeSource)
    {
        std::wstring filename = m_shaderDirectory + pFilename;
        ComPtr<IDxcBlobEncoding> pBlob;
        HRESULT hr = g_pUtils->LoadFile(filename.c_str(), nullptr, &pBlob);
        if (FAILED(hr))
        {
            const std::wstring_view name = StripRelativePrefix(pFilename);
            const EmbeddedHeader* match = nullptr;
            for (const EmbeddedHeader& header : kEmbeddedHeaders)
            {
                if (name == header.Name)
                {
                    match = &header;
                    break;
                }
            }

            if (match == nullptr)
            {
                // Serving inc_common for an unrecognised name is the historical behaviour and is
                // kept so nothing that works today breaks -- but it silently compiles the wrong
                // source, so say so rather than leaving the next typo to be debugged blind.
                DebugConsole::LogError("Shader include not found and not embedded: '"
                    + std::filesystem::path(name).string() + "'. Substituting inc_common.hlsl.");
                match = &kEmbeddedHeaders[0];
            }

            ThrowIfFailed(g_pUtils->CreateBlob(match->Data, match->Size, DXC_CP_UTF8, &pBlob));
        }
        *ppIncludeSource = pBlob.Detach();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ShaderIncludeHandler::QueryInterface(REFIID riid, void** ppvObject)
    {
        if (riid == __uuidof(IDxcIncludeHandler)) {
            *ppvObject = static_cast<IDxcIncludeHandler*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE ShaderIncludeHandler::AddRef()
    {
        return InterlockedIncrement(&m_refCount);
    }

    ULONG STDMETHODCALLTYPE ShaderIncludeHandler::Release()
    {
        ULONG refCount = InterlockedDecrement(&m_refCount);
        if (refCount == 0) {
            delete this;
        }
        return refCount;
    }

    ComPtr<IDxcBlob> CompileShader(const std::wstring& filename, const std::span<std::wstring>& defines, const std::wstring& entrypoint, const std::wstring& target)
    {
        if (!g_pUtils) {
            ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&g_pUtils)));
            ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&g_pCompiler)));
        }

        ComPtr<IDxcIncludeHandler> pIncludeHandler{
            new ShaderIncludeHandler(filename.substr(0, filename.find_last_of(L"\\/") + 1))
        };

        ComPtr<IDxcBlobEncoding> pBlob;
        ThrowIfFailed(g_pUtils->LoadFile(filename.c_str(), nullptr, &pBlob));

        DxcBuffer source;
        source.Ptr = pBlob->GetBufferPointer();
        source.Size = pBlob->GetBufferSize();
        source.Encoding = DXC_CP_ACP;

        std::vector<LPCWSTR> args{
            L"-E", entrypoint.c_str(),
            L"-T", target.c_str(),
#ifdef PROFILE_ENABLE
            L"-Zi",
            L"-Qembed_debug"
#endif
        };

        for (const auto& define : defines) {
            args.push_back(L"-D");
            args.push_back(define.c_str());
        }

        ComPtr<IDxcResult> pResult;
        ThrowIfFailed(g_pCompiler->Compile(&source, args.data(), static_cast<UINT32>(args.size()), pIncludeHandler.Get(), IID_PPV_ARGS(&pResult)));

        ComPtr<IDxcBlobUtf8> pErrors;
        ThrowIfFailed(pResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&pErrors), nullptr));
        if (pErrors && pErrors->GetStringLength() > 0) {
            DebugConsole::Log(std::string((char*)pErrors->GetBufferPointer()));
        }

        HRESULT hrStatus;
        ThrowIfFailed(pResult->GetStatus(&hrStatus));
        ThrowIfFailed(hrStatus);

        ComPtr<IDxcBlob> pObject;
        ThrowIfFailed(pResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&pObject), nullptr));
        return pObject;
    }

    bool ShaderHasEntryPoint(const std::wstring& filename, const std::span<std::wstring>& defines, const std::wstring& entrypoint)
    {
        if (!g_pUtils) {
            ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&g_pUtils)));
            ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&g_pCompiler)));
        }

        ComPtr<IDxcIncludeHandler> pIncludeHandler{
            new ShaderIncludeHandler(filename.substr(0, filename.find_last_of(L"\\/") + 1))
        };

        ComPtr<IDxcBlobEncoding> pBlob;
        ThrowIfFailed(g_pUtils->LoadFile(filename.c_str(), nullptr, &pBlob));

        DxcBuffer source;
        source.Ptr = pBlob->GetBufferPointer();
        source.Size = pBlob->GetBufferSize();
        source.Encoding = DXC_CP_ACP;

        // Preprocess only (-P): expands #include and resolves #ifdef under the same defines, without
        // requiring an entry point, so no "entry point not found" diagnostic is ever produced.
        std::vector<LPCWSTR> args{ L"-P" };
        for (const auto& define : defines) {
            args.push_back(L"-D");
            args.push_back(define.c_str());
        }

        ComPtr<IDxcResult> pResult;
        ThrowIfFailed(g_pCompiler->Compile(&source, args.data(), static_cast<UINT32>(args.size()), pIncludeHandler.Get(), IID_PPV_ARGS(&pResult)));

        HRESULT hrStatus;
        ThrowIfFailed(pResult->GetStatus(&hrStatus));
        ThrowIfFailed(hrStatus);

        ComPtr<IDxcBlobUtf8> pHlsl;
        ThrowIfFailed(pResult->GetOutput(DXC_OUT_HLSL, IID_PPV_ARGS(&pHlsl), nullptr));
        if (!pHlsl || pHlsl->GetStringLength() == 0) {
            return false;
        }

        std::string_view preprocessed((char*)pHlsl->GetBufferPointer(), pHlsl->GetStringLength());

        std::string name; // entry-point names are ASCII; narrow explicitly to avoid a conversion warning
        name.reserve(entrypoint.size());
        for (wchar_t c : entrypoint) {
            name.push_back(static_cast<char>(c));
        }
        return SourceDefinesFunction(preprocessed, name);
    }

    ComPtr<IDxcBlob> CompileShaderFromMemory(const std::string& data, const std::span<std::wstring>& defines, const std::wstring& entrypoint, const std::wstring& target)
    {
        if (!g_pUtils) {
            ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&g_pUtils)));
            ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&g_pCompiler)));
		}

        DxcBuffer source;
        source.Ptr = data.c_str();
        source.Size = data.size();
        source.Encoding = DXC_CP_ACP;

        std::vector<LPCWSTR> args{
            L"-E", entrypoint.c_str(),
            L"-T", target.c_str(),
#ifdef PROFILE_ENABLE
            L"-Zi",
            L"-Qembed_debug"
#endif
        };

        for (const auto& define : defines) {
			args.push_back(L"-D");
			args.push_back(define.c_str());
		}

        ComPtr<IDxcResult> pResult;
        ThrowIfFailed(g_pCompiler->Compile(&source, args.data(), static_cast<UINT32>(args.size()), nullptr, IID_PPV_ARGS(&pResult)));

        ComPtr<IDxcBlobUtf8> pErrors;
        ThrowIfFailed(pResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&pErrors), nullptr));
        if (pErrors && pErrors->GetStringLength() > 0) {
            DebugConsole::Log(std::string((char*)pErrors->GetBufferPointer()));
        }

        HRESULT hrStatus;
        ThrowIfFailed(pResult->GetStatus(&hrStatus));
        ThrowIfFailed(hrStatus);

        ComPtr<IDxcBlob> pObject;
        ThrowIfFailed(pResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&pObject), nullptr));
        return pObject;
    }
}