#include "pch.h"
#include "core.h"

#include "singleton.h"
#include "resource.h"
#include "input.h"
#include "frame_resource.h"
#include "time_measure.h"
#include "scene.h"
#include "mesh.h"
#include "shader.h"
#include "frame_debug.h"

namespace udsdx
{
	Core::Core()
	{

	}

	Core::~Core()
	{
	}

	void Core::Initialize(HINSTANCE hInstance, HWND hWnd)
	{ ZoneScoped;
		m_hInstance = hInstance;
		m_hMainWnd = hWnd;

		m_timeMeasure = std::make_unique<TimeMeasure>();
		m_timeMeasure->BeginMeasure();

		m_windowedRect = { 0, 0, m_clientWidth, m_clientHeight };

#if _WIN32_WINNT >= 0x0A00 // _WIN32_WINNT_WIN10
		m_roInitialization = std::make_unique<Wrappers::RoInitializeWrapper>(RO_INIT_MULTITHREADED);
		assert(SUCCEEDED(*m_roInitialization));
#else
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		assert(SUCCEEDED(hr));
#endif
		// Get title of window
		wchar_t buffer[256];
		GetWindowText(m_hMainWnd, buffer, 256);
		m_mainWndCaption = buffer;

		InitializeDirect3D();
		OnResizeWindow(m_clientWidth, m_clientHeight);

		// Reset the command list to prep for initialization commands.
		ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

		for (int i = 0; i < FrameResourceCount; ++i)
		{
			m_frameResources[i] = std::make_unique<FrameResource>(m_d3dDevice.Get());
		}

		BuildDescriptorHeaps();
		BuildConstantBuffers();
		BuildRootSignature();

		INSTANCE(Resource)->Initialize(m_d3dDevice.Get(), m_commandList.Get(), m_rootSignature.Get());

		ThrowIfFailed(m_commandList->Close());
		ID3D12CommandList* cmdsLists[] = { m_commandList.Get() };
		m_commandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

		m_fenceEvent = ::CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
		FlushCommandQueue();

		// Create frame debug window
		m_frameDebug = std::make_unique<FrameDebug>(m_hInstance);
	}

	void Core::InitializeDirect3D()
	{ ZoneScoped;
#if defined(DEBUG) || defined(_DEBUG) 
		ThrowIfFailed(::D3D12GetDebugInterface(IID_PPV_ARGS(&m_debugController)));
		m_debugController->EnableDebugLayer();
#endif

		// Create DXGI Factory
		ThrowIfFailed(::CreateDXGIFactory1(IID_PPV_ARGS(&m_dxgiFactory)));
		// Create hardware device
		HRESULT hr = ::D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_d3dDevice));

		// Fallback to WARP device
		if (FAILED(hr))
		{
			ComPtr<IDXGIAdapter> adapter = nullptr;
			ThrowIfFailed(m_dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));

			// Create hardware device with WARP adapter
			ThrowIfFailed(::D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_d3dDevice)));
		}

		m_tearingSupport = CheckTearingSupport();

		// Create command queue
		ThrowIfFailed(m_d3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));

		m_rtvDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		m_dsvDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
		m_cbvSrvUavDescriptorSize = m_d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		// Check 4X MSAA quality support for our back buffer format.
		D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
		msQualityLevels.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		msQualityLevels.SampleCount = 4;
		msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
		msQualityLevels.NumQualityLevels = 0;

		ThrowIfFailed(m_d3dDevice->CheckFeatureSupport(
			D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
			&msQualityLevels,
			sizeof(msQualityLevels)
		));

		m_4xMsaaQuality = msQualityLevels.NumQualityLevels;
		assert(m_4xMsaaQuality > 0 && "Unexpected MSAA quality level.");


#if defined(DEBUG) || defined(_DEBUG)
		PrintAdapterInfo();
#endif
		CreateCommandObjects();
		CreateSwapChain();
		CreateRtvAndDsvDescriptorHeaps();

		const char tracyQueueName[] = "D3D12 Graphics Queue";
		m_tracyQueueCtx = TracyD3D12Context(m_d3dDevice.Get(), m_commandQueue.Get());
		TracyD3D12ContextName(m_tracyQueueCtx, tracyQueueName, sizeof(tracyQueueName));

		if (m_tearingSupport)
		{
			m_dxgiFactory->MakeWindowAssociation(m_hMainWnd, DXGI_MWA_NO_ALT_ENTER);
		}
	}

	void Core::CreateCommandObjects()
	{ ZoneScoped;
		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

		ThrowIfFailed(m_d3dDevice->CreateCommandQueue(
			&queueDesc,
			IID_PPV_ARGS(&m_commandQueue)
		));
		ThrowIfFailed(m_d3dDevice->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			IID_PPV_ARGS(m_directCmdListAlloc.GetAddressOf())
		));
		ThrowIfFailed(m_d3dDevice->CreateCommandList(
			0,
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			m_directCmdListAlloc.Get(),
			nullptr,
			IID_PPV_ARGS(m_commandList.GetAddressOf())
		));

		m_commandList->Close();
	}

	void Core::RegisterUpdateCallback(std::function<void(const Time&)> callback)
	{
		m_updateCallback = callback;
	}

	void Core::CreateSwapChain()
	{ ZoneScoped;
		// Release the previous swapchain we will be recreating.
		m_swapChain.Reset();

		DXGI_SWAP_CHAIN_DESC sd;
		sd.BufferDesc.Width = m_clientWidth;
		sd.BufferDesc.Height = m_clientHeight;
		sd.BufferDesc.RefreshRate.Numerator = 60;
		sd.BufferDesc.RefreshRate.Denominator = 1;
		sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
		sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;

		// Use 4X MSAA
		if (m_4xMsaaState)
		{
			sd.SampleDesc.Count = 4;
			sd.SampleDesc.Quality = m_4xMsaaQuality - 1;
		}
		// No MSAA
		else
		{
			sd.SampleDesc.Count = 1;
			sd.SampleDesc.Quality = 0;
		}

		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = SwapChainBufferCount;
		sd.OutputWindow = m_hMainWnd;
		sd.Windowed = true;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd.Flags = m_tearingSupport ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

		// Note: Swap chain uses queue to perform flush.
		ThrowIfFailed(m_dxgiFactory->CreateSwapChain(m_commandQueue.Get(), &sd, m_swapChain.GetAddressOf()));
	}

	void Core::CreateRtvAndDsvDescriptorHeaps()
	{ ZoneScoped;
		D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
		rtvHeapDesc.NumDescriptors = SwapChainBufferCount;
		rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		rtvHeapDesc.NodeMask = 0;
		ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(m_rtvHeap.GetAddressOf())));

		D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
		dsvHeapDesc.NumDescriptors = 1;
		dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		dsvHeapDesc.NodeMask = 0;
		ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(m_dsvHeap.GetAddressOf())));
	}

	void Core::OnDestroy()
	{ ZoneScoped;
		if (m_d3dDevice != nullptr)
		{
			// Ensure that the GPU is no longer referencing resources that are about to be destroyed.
			FlushCommandQueue();
			::CloseHandle(m_fenceEvent);

			// Release the context
			DestroyD3D12Context(m_tracyQueueCtx);
		}
	}

	void Core::BuildDescriptorHeaps()
	{ ZoneScoped;
		// Need a CBV descriptor for each object for each frame resource,

		D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc;
		cbvHeapDesc.NumDescriptors = FrameResourceCount;
		cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		cbvHeapDesc.NodeMask = 0;
		ThrowIfFailed(m_d3dDevice->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(m_cbvHeap.GetAddressOf())));
	}

	void Core::BuildConstantBuffers()
	{ ZoneScoped;
		UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

		for (int frameIndex = 0; frameIndex < FrameResourceCount; ++frameIndex)
		{
			auto objectCB = m_frameResources[frameIndex]->GetObjectCB();
			D3D12_GPU_VIRTUAL_ADDRESS cbAddress = objectCB->Resource()->GetGPUVirtualAddress();

			// Offset to the ith object constant buffer in the buffer.
			auto handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_cbvHeap->GetCPUDescriptorHandleForHeapStart());
			handle.Offset(frameIndex, m_cbvSrvUavDescriptorSize);

			D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
			cbvDesc.BufferLocation = cbAddress;
			cbvDesc.SizeInBytes = objCBByteSize;

			m_d3dDevice->CreateConstantBufferView(&cbvDesc, handle);
		}
	}

	void Core::BuildRootSignature()
	{ ZoneScoped;
		CD3DX12_ROOT_PARAMETER slotRootParameter[3];

		CD3DX12_DESCRIPTOR_RANGE cbvTable;
		cbvTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 2);

		slotRootParameter[0].InitAsConstants(16, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
		slotRootParameter[1].InitAsConstants(20, 1, 0, D3D12_SHADER_VISIBILITY_VERTEX);
		slotRootParameter[2].InitAsDescriptorTable(1, &cbvTable);

		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter, 0, nullptr,
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
		);

		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(
			&rootSigDesc,
			D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(),
			errorBlob.GetAddressOf()
		);

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(m_d3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(m_rootSignature.GetAddressOf())
		));
	}

	bool Core::CheckTearingSupport() const
	{
#ifndef PIXSUPPORT
		ComPtr<IDXGIFactory6> factory;
		HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
		BOOL allowTearing = FALSE;
		if (SUCCEEDED(hr))
		{
			hr = factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
		}

		return SUCCEEDED(hr) && allowTearing;
#else
		return true;
#endif
	}

	void Core::PrintAdapterInfo()
	{
		// Output information
		std::wcout << L"DXGI Adapters:\n";
		IDXGIAdapter* adapter = nullptr;
		for (UINT i = 0; m_dxgiFactory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
		{
			DXGI_ADAPTER_DESC desc;
			adapter->GetDesc(&desc);

			std::wstring text = std::format(L"> Adapter: {}\n  Output:\n", desc.Description);
			std::wcout << text;

			IDXGIOutput* m_output = nullptr;
			for (UINT j = 0; adapter->EnumOutputs(j, &m_output) != DXGI_ERROR_NOT_FOUND; ++j)
			{
				DXGI_OUTPUT_DESC desc;
				m_output->GetDesc(&desc);

				std::wstring text = std::format(L"   > {}\n", desc.DeviceName);
				std::wcout << text;

				m_output->Release();
			}
			adapter->Release();
		}
		std::cout << std::endl;
	}

	void Core::DisplayFrameStats()
	{
		constexpr int RefreshRate = 15;
		static int frameCount = 0;

		static std::chrono::steady_clock timer;
		static std::chrono::steady_clock::time_point lc;
		std::chrono::steady_clock::time_point c = timer.now();

		double delta = std::chrono::duration_cast<std::chrono::duration<double>>(c - lc).count();
		if (delta * RefreshRate > 1.0)
		{
			int fps = static_cast<int>(round(1.0 / delta * frameCount));
			SetWindowText(m_hMainWnd, (m_mainWndCaption + std::format(L" [{} x {}] @ {} FPS", m_clientWidth, m_clientHeight, fps)).c_str());
			frameCount = 0;
			lc = c;
		}
		frameCount += 1;
	}

	void Core::FlushCommandQueue()
	{ ZoneScoped;
		// Advance the fence value to mark commands up to this fence point.
		m_currentFence++;

		// Add an instruction to the command queue to set a new fence point. Because we are on the GPU time line, the new fence point won't be set until the GPU finishes processing all the commands prior to this Signal().
		ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_currentFence));

		// Wait until the GPU has completed commands up to this fence point.
		if (m_fence->GetCompletedValue() < m_currentFence)
		{
			// Fire event when GPU hits current fence.
			ThrowIfFailed(m_fence->SetEventOnCompletion(m_currentFence, m_fenceEvent));

			// Wait until the GPU hits current fence event is fired.
			::WaitForSingleObject(m_fenceEvent, INFINITE);
		}
	}

	void Core::SetScene(std::shared_ptr<Scene> scene)
	{
		m_scene = scene;
	}

	void Core::AcquireNextFrameResource()
	{ ZoneScopedC(0x249EA0);
		m_currFrameResourceIndex = (m_currFrameResourceIndex + 1) % FrameResourceCount;
		auto frameResource = CurrentFrameResource();

		if (frameResource->GetFence() != 0 && m_fence->GetCompletedValue() < frameResource->GetFence())
		{
			// Fire event when GPU hits current fence.
			ThrowIfFailed(m_fence->SetEventOnCompletion(frameResource->GetFence(), m_fenceEvent));

			// Wait until the GPU hits current fence event is fired.
			::WaitForSingleObject(m_fenceEvent, INFINITE);
		}
	}

	void Core::Update()
	{ ZoneScopedC(0xFAAB36);
		DisplayFrameStats();

		m_timeMeasure->EndMeasure();
		m_timeMeasure->BeginMeasure();

		INSTANCE(Input)->FlushQueue();

		BroadcastUpdateMessage();
		m_scene->Update(m_timeMeasure->GetTime());

		// Update the constant buffer with the latest view and project matrix.
		UpdateMainPassCB();
	}

	void Core::BroadcastUpdateMessage()
	{ ZoneScoped;
		if (m_updateCallback)
		{
			m_updateCallback(m_timeMeasure->GetTime());
		}
	}

	void Core::Render()
	{ ZoneScopedC(0xF78104);
		TracyD3D12Collect(m_tracyQueueCtx);
		TracyD3D12NewFrame(m_tracyQueueCtx);
		if (m_resizeDirty)
		{
			m_resizeDirty = false;
			OnResizeWindow(m_clientWidth, m_clientHeight);
		}

		auto frameResource = CurrentFrameResource();
		auto cmdListAlloc = frameResource->GetCommandListAllocator();

		ThrowIfFailed(cmdListAlloc->Reset());
		// Resets a command list back to its initial state as if a new command list was just created.
		// ID3D12PipelineState: This is optional and can be NULL. If NULL, the runtime sets a dummy initial pipeline state so that drivers don't have to deal with undefined state.
		ThrowIfFailed(m_commandList->Reset(cmdListAlloc, nullptr));

		m_commandList->RSSetViewports(1, &m_screenViewport);
		m_commandList->RSSetScissorRects(1, &m_scissorRect);

		// Indicate a state transition on the resource usage.
		m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			CurrentBackBuffer(),
			D3D12_RESOURCE_STATE_PRESENT,
			D3D12_RESOURCE_STATE_RENDER_TARGET
		));

		{ TracyD3D12Zone(m_tracyQueueCtx, m_commandList.Get(), "Clear Buffers");
		// Clear the back buffer and depth buffer.
		m_commandList->ClearRenderTargetView(
			CurrentBackBufferView(),
			(float*)&m_clearColor,
			0,
			nullptr
		);
		m_commandList->ClearDepthStencilView(
			DepthStencilView(),
			D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
			1.0f,
			0,
			0,
			nullptr
		);

		// Specify the buffers we are going to render to.
		m_commandList->OMSetRenderTargets(
			1,
			&CurrentBackBufferView(),
			true,
			&DepthStencilView()
		);
		}

		ID3D12DescriptorHeap* descriptorHeaps[] = { m_cbvHeap.Get() };
		m_commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
		m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());

		// Bind the current frame's constant buffer to the pipeline.
		auto passCbvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_cbvHeap->GetGPUDescriptorHandleForHeapStart());
		passCbvHandle.Offset(m_currFrameResourceIndex, m_cbvSrvUavDescriptorSize);
		m_commandList->SetGraphicsRootDescriptorTable(2, passCbvHandle);

		float aspect = static_cast<float>(m_clientWidth) / m_clientHeight;

		{ TracyD3D12Zone(m_tracyQueueCtx, m_commandList.Get(), "Draw Calls");
		// Draw the scene objects. 
		m_scene->Render(*m_commandList.Get(), aspect);
		}

		// indicate a state transition on the resource usage.
		m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			CurrentBackBuffer(),
			D3D12_RESOURCE_STATE_RENDER_TARGET,
			D3D12_RESOURCE_STATE_PRESENT
		));

		// Done recording commands.
		ThrowIfFailed(m_commandList->Close());

		ID3D12CommandList* cmdLists[] = { m_commandList.Get() };
		m_commandQueue->ExecuteCommandLists(_countof(cmdLists), cmdLists);

		// When using sync interval 0, it is recommended to always pass the tearing
		// flag when it is supported, even when presenting in windowed mode.
		// However, this flag cannot be used if the app is in fullscreen mode as a
		// result of calling SetFullscreenState.
		UINT presentFlags = (m_tearingSupport && !m_fullscreen) ? DXGI_PRESENT_ALLOW_TEARING : 0;

		{ ZoneScopedN("Present Swap Chain");
			// Swap the back and front buffers
			ThrowIfFailed(m_swapChain->Present(0, presentFlags));
			m_currBackBuffer = (m_currBackBuffer + 1) % SwapChainBufferCount;
		}

		// Advance the fence value to mark commands up to this fence point.
		frameResource->SetFence(++m_currentFence);
		m_commandQueue->Signal(m_fence.Get(), m_currentFence);

		// Frame debug
		m_frameDebug->Update(m_timeMeasure->GetTime());
	}

	void Core::UpdateMainPassCB()
	{ ZoneScoped;
		PassConstants passConstants;
		passConstants.TotalTime = m_timeMeasure->GetTime().totalTime;

		auto frameResource = CurrentFrameResource();
		frameResource->GetObjectCB()->CopyData(0, passConstants);
	}

	bool Core::ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
	{ ZoneScoped;
		switch (message)
		{
		case WM_SIZE:
			// Save the new client area dimensions.
			m_clientWidth = LOWORD(lParam);
			m_clientHeight = HIWORD(lParam);

			// Notify the display associated resources for the resize event.
			m_resizeDirty = true;
			break;

		// Catch this message so to prevent the window from becoming too small.
		case WM_GETMINMAXINFO:
			((MINMAXINFO*)lParam)->ptMinTrackSize.x = m_minClientWidth;
			((MINMAXINFO*)lParam)->ptMinTrackSize.y = m_minClientHeight;
			break;

		case WM_SYSCHAR:
			// Handle Alt + Enter key sequence
			if ((wParam == VK_RETURN) && (lParam & (1 << 29)))
			{
				if (m_d3dDevice && m_tearingSupport)
				{
					SetWindowFullscreen(!m_fullscreen);
					break;
				}
			}
			return false;

		default:
			return INSTANCE(Input)->ProcessMessage(hWnd, message, wParam, lParam);
		}
		
		return true;
	}

	void Core::SetWindowFullscreen(bool fullscreen)
	{
		if (fullscreen == m_fullscreen)
		{
			return;
		}

		if (!m_tearingSupport)
		{
			return;
		}

		m_fullscreen = fullscreen;
		if (fullscreen)
		{
			GetWindowRect(m_hMainWnd, &m_windowedRect);
			SetWindowLong(m_hMainWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW & ~(WS_CAPTION | WS_MAXIMIZEBOX | WS_MINIMIZEBOX | WS_SYSMENU | WS_THICKFRAME));

			RECT windowRect;
			if (m_swapChain)
			{
				ComPtr<IDXGIOutput> pOutput;
				ThrowIfFailed(m_swapChain->GetContainingOutput(pOutput.GetAddressOf()));
				DXGI_OUTPUT_DESC desc;
				ThrowIfFailed(pOutput->GetDesc(&desc));
				windowRect = desc.DesktopCoordinates;
			}
			// Fallback to EnumDisplaySettings implementation
			else
			{
				// Get the settings of the primary display
				DEVMODE devMode = {};
				devMode.dmSize = sizeof(DEVMODE);
				EnumDisplaySettings(nullptr, ENUM_CURRENT_SETTINGS, &devMode);

				windowRect = {
					devMode.dmPosition.x,
					devMode.dmPosition.y,
					devMode.dmPosition.x + static_cast<LONG>(devMode.dmPelsWidth),
					devMode.dmPosition.y + static_cast<LONG>(devMode.dmPelsHeight)
				};
			}

			SetWindowPos(
				m_hMainWnd,
				HWND_TOP,
				windowRect.left,
				windowRect.top,
				windowRect.right,
				windowRect.bottom,
				SWP_FRAMECHANGED | SWP_NOACTIVATE
			);
			ShowWindow(m_hMainWnd, SW_MAXIMIZE);
		}
		else
		{
			SetWindowLong(m_hMainWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);

			SetWindowPos(
				m_hMainWnd,
				HWND_NOTOPMOST,
				m_windowedRect.left,
				m_windowedRect.top,
				m_windowedRect.right - m_windowedRect.left,
				m_windowedRect.bottom - m_windowedRect.top,
				SWP_FRAMECHANGED | SWP_NOACTIVATE
			);
			ShowWindow(m_hMainWnd, SW_NORMAL);
		}
	}

	bool Core::OnResizeWindow(int width, int height)
	{ ZoneScoped;
		if (width <= 0 || height <= 0)
		{
			return false;
		}

		assert(m_d3dDevice);
		assert(m_swapChain);
		assert(m_directCmdListAlloc);

		// Flush before changing any resources.
		FlushCommandQueue();

		ThrowIfFailed(m_commandList->Reset(m_directCmdListAlloc.Get(), nullptr));

		// Release the previous resources we will be recreating.
		for (int i = 0; i < SwapChainBufferCount; ++i)
		{
			m_swapChainBuffers[i].Reset();
		}
		m_depthStencilBuffer.Reset();

		// Get the description of the swap chain.
		DXGI_SWAP_CHAIN_DESC swapChainDesc;
		m_swapChain->GetDesc(&swapChainDesc);

		// Resize the swap chain.
		ThrowIfFailed(m_swapChain->ResizeBuffers(
			SwapChainBufferCount,
			width, height,
			swapChainDesc.BufferDesc.Format,
			swapChainDesc.Flags
		));

		m_currBackBuffer = 0;

		CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
		for (UINT i = 0; i < SwapChainBufferCount; i++)
		{
			ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_swapChainBuffers[i])));
			m_d3dDevice->CreateRenderTargetView(m_swapChainBuffers[i].Get(), nullptr, rtvHeapHandle);
			rtvHeapHandle.Offset(1, m_rtvDescriptorSize);
		}

		// Create the depth/stencil buffer and view.
		D3D12_RESOURCE_DESC depthStencilDesc;
		depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		depthStencilDesc.Alignment = 0;
		depthStencilDesc.Width = width;
		depthStencilDesc.Height = height;
		depthStencilDesc.DepthOrArraySize = 1;
		depthStencilDesc.MipLevels = 1;

		// Correction 11/12/2016: SSAO chapter requires an SRV to the depth buffer to read from 
		// the depth buffer.  Therefore, because we need to create two views to the same resource:
		//   1. SRV format: DXGI_FORMAT_R24_UNORM_X8_TYPELESS
		//   2. DSV Format: DXGI_FORMAT_D24_UNORM_S8_UINT
		// we need to create the depth buffer resource with a typeless format.  
		depthStencilDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;

		depthStencilDesc.SampleDesc.Count = m_4xMsaaState ? 4 : 1;
		depthStencilDesc.SampleDesc.Quality = m_4xMsaaState ? (m_4xMsaaQuality - 1) : 0;
		depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

		D3D12_CLEAR_VALUE optClear;
		optClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		optClear.DepthStencil.Depth = 1.0f;
		optClear.DepthStencil.Stencil = 0;
		ThrowIfFailed(m_d3dDevice->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&depthStencilDesc,
			D3D12_RESOURCE_STATE_COMMON,
			&optClear,
			IID_PPV_ARGS(m_depthStencilBuffer.GetAddressOf())
		));

		// Create descriptor to mip level 0 of entire resource using the format of the resource.
		D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
		dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
		dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		dsvDesc.Texture2D.MipSlice = 0;
		m_d3dDevice->CreateDepthStencilView(m_depthStencilBuffer.Get(), &dsvDesc, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

		// Transition the resource from its initial state to be used as a depth buffer.
		m_commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
			m_depthStencilBuffer.Get(),
			D3D12_RESOURCE_STATE_COMMON,
			D3D12_RESOURCE_STATE_DEPTH_WRITE
		));

		// Execute the resize commands.
		ThrowIfFailed(m_commandList->Close());
		ID3D12CommandList* cmdsLists[] = { m_commandList.Get() };
		m_commandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

		// Wait until resize is complete.
		FlushCommandQueue();

		// Update the viewport transform to cover the client area.
		m_screenViewport.TopLeftX = 0;
		m_screenViewport.TopLeftY = 0;
		m_screenViewport.Width = static_cast<float>(width);
		m_screenViewport.Height = static_cast<float>(height);
		m_screenViewport.MinDepth = 0.0f;
		m_screenViewport.MaxDepth = 1.0f;

		m_scissorRect = { 0, 0, width, height };

		return true;
	}

	ID3D12Resource* Core::CurrentBackBuffer() const
	{
		return m_swapChainBuffers[m_currBackBuffer].Get();
	}

	D3D12_CPU_DESCRIPTOR_HANDLE Core::CurrentBackBufferView() const
	{
		return CD3DX12_CPU_DESCRIPTOR_HANDLE(
			m_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
			m_currBackBuffer,
			m_rtvDescriptorSize
		);
	}

	FrameResource* Core::CurrentFrameResource() const
	{
		return m_frameResources[m_currFrameResourceIndex].get();
	}

	int Core::GetClientWidth() const
	{
		return m_clientWidth;
	}

	int Core::GetClientHeight() const
	{
		return m_clientHeight;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE Core::DepthStencilView() const
	{
		return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
	}
}
