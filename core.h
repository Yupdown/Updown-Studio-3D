#pragma once

#include "pch.h"

namespace udsdx
{
	class FrameDebug;
	class FrameResource;
	class TimeMeasure;
	class Scene;
	class LightDirectional;

	class Core
	{
	public:
		Core();
		Core(const Core& rhs) = delete;
		Core& operator=(const Core& rhs) = delete;
		virtual ~Core();

		void Initialize(HINSTANCE hInstance, HWND hWnd);
		void InitializeDirect3D();
		bool CheckTearingSupport() const;
		void PrintAdapterInfo();
		void DisplayFrameStats();
		void OnDestroy();

		void CreateCommandObjects();
		void CreateSwapChain();
		void CreateRtvAndDsvDescriptorHeaps();

		void BuildDescriptorHeaps();
		void BuildConstantBuffers();
		void BuildRootSignature();

		void ExecuteCommandList();
		void FlushCommandQueue();

		void SetScene(std::shared_ptr<Scene> scene);
		void RegisterUpdateCallback(std::function<void(const Time&)> callback);
		void AcquireNextFrameResource();
		void Update();
		void BroadcastUpdateMessage();
		void Render();
		void UpdateMainPassCB();
		void SetWindowFullscreen(bool fullscreen);
		bool ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
		virtual bool OnResizeWindow(int width, int height);

	public:
		ID3D12Device* GetDevice() const;
		ID3D12CommandQueue* GetCommandQueue() const;
		ID3D12CommandAllocator* GetCommandAllocator() const;
		ID3D12GraphicsCommandList* GetCommandList() const;

		FrameResource* CurrentFrameResource() const;
		ID3D12Resource* CurrentBackBuffer() const;

		D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView() const;
		D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const;

		int GetClientWidth() const;
		int GetClientHeight() const;

		void SetClearColor(const Color& clearColor);
		void SetClearColor(float r, float g, float b);

	protected:
		HINSTANCE	m_hInstance = 0;
		HWND		m_hMainWnd = 0;

		std::wstring m_mainWndCaption;

		int			m_clientWidth = 800;
		int			m_clientHeight = 600;

		int			m_minClientWidth = 240;
		int 	    m_minClientHeight = 240;

		bool		m_fullscreen = false;

		std::atomic_bool m_resizeDirty = false;

		RECT		m_windowedRect;

		bool		m_tearingSupport = false;
		// Set true to use 4X MSAA (?.1.8).  The default is false.
		bool		m_4xMsaaState = false;    // 4X MSAA enabled
		UINT		m_4xMsaaQuality = 0;      // quality level of 4X MSAA

		Color    m_clearColor = Color(0.0f, 0.0f, 0.0f, 1.0f);

		// Current Scene to render with
		std::shared_ptr<Scene> m_scene;
		std::function<void(const Time&)> m_updateCallback;

	protected:
		std::unique_ptr<TimeMeasure> m_timeMeasure;
		std::unique_ptr<Wrappers::RoInitializeWrapper> m_roInitialization;

		// Factory for creating DXGI objects
		ComPtr<IDXGIFactory4> m_dxgiFactory;
		// Direct3D 12 Device
		ComPtr<ID3D12Device> m_d3dDevice;
		// Swap Chain (front and back buffer, similar as double-buffering)
		ComPtr<IDXGISwapChain> m_swapChain;
		// Direct3D 12 Debug Layer
		ComPtr<ID3D12Debug> m_debugController;
		// Fence for CPU/GPU synchronization
		ComPtr<ID3D12Fence> m_fence;
		UINT64 m_currentFence = 0;
		HANDLE m_fenceEvent = nullptr;

		ComPtr<ID3D12CommandQueue> m_commandQueue;
		ComPtr<ID3D12CommandAllocator> m_directCmdListAlloc;
		ComPtr<ID3D12GraphicsCommandList> m_commandList;

		static constexpr int FrameResourceCount = 3;
		std::array<std::unique_ptr<FrameResource>, FrameResourceCount> m_frameResources;
		int m_currFrameResourceIndex = 0;

		static constexpr int SwapChainBufferCount = 2;
		std::array<ComPtr<ID3D12Resource>, SwapChainBufferCount> m_swapChainBuffers;
		int m_currBackBuffer = 0;

		ComPtr<ID3D12Resource> m_depthStencilBuffer;

		// Render Target View Descriptor Heap
		ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
		// Depth/Stencil View Descriptor Heap
		ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
		// Constant Buffer View Descriptor Heap
		ComPtr<ID3D12DescriptorHeap> m_cbvHeap;
		// Shader Resource View Descriptor Heap
		ComPtr<ID3D12DescriptorHeap> m_srvHeap;

		// Root Signature:
		// used to define the data that the shaders will access
		ComPtr<ID3D12RootSignature> m_rootSignature;

		D3D12_VIEWPORT m_screenViewport;
		D3D12_RECT m_scissorRect;

		UINT m_rtvDescriptorSize = 0;
		UINT m_dsvDescriptorSize = 0;
		UINT m_cbvSrvUavDescriptorSize = 0;

		std::unique_ptr<FrameDebug> m_frameDebug;
		TracyD3D12Ctx m_tracyQueueCtx;

		std::unique_ptr<LightDirectional> m_directionalLight;
	};
}

