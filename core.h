#pragma once

#include "pch.h"

namespace udsdx
{
	class FrameResource;
	class TimeMeasure;

	class Core
	{
	protected:
		HINSTANCE	m_hInstance = 0;
		HWND		m_hMainWnd = 0;

		std::wstring m_mainWndCaption;

		int			m_clientWidth = 800;
		int			m_clientHeight = 600;

		int			m_minClientWidth = 240;
		int 	    m_minClientHeight = 240;

		bool		m_fullscreen = false;

		RECT		m_windowedRect;

		bool		m_tearingSupport = false;
		// Set true to use 4X MSAA (?.1.8).  The default is false.
		bool		m_4xMsaaState = false;    // 4X MSAA enabled
		UINT		m_4xMsaaQuality = 0;      // quality level of 4X MSAA

		XMFLOAT4    m_clearColor = { 0.125f, 0.125f, 0.125f, 1.0f }; // #1F7B8F

		// scale of xyz = 0.1f;
		XMFLOAT4X4	m_world = MathHelper::Identity4x4();
		XMFLOAT4X4	m_view = MathHelper::Identity4x4();
		XMFLOAT4X4	m_proj = MathHelper::Identity4x4();

		float m_theta = 1.5f * XM_PI;
		float m_phi = XM_PIDIV4;
		float m_radius = 20.0f;

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

		ComPtr<ID3D12Fence> m_fence;
		UINT64 m_currentFence = 0;

		ComPtr<ID3D12CommandQueue> m_commandQueue;
		ComPtr<ID3D12CommandAllocator> m_directCmdListAlloc;
		ComPtr<ID3D12GraphicsCommandList> m_commandList;

		static constexpr int FrameResourceCount = 3;
		std::unique_ptr<FrameResource> m_frameResources[FrameResourceCount];
		int m_currFrameResourceIndex = 0;

		static constexpr int SwapChainBufferCount = 2;
		int m_currBackBuffer = 0;

		ComPtr<ID3D12Resource> m_swapChainBuffer[SwapChainBufferCount];
		ComPtr<ID3D12Resource> m_depthStencilBuffer;

		// Render Target View Descriptor Heap
		ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
		// Depth/Stencil View Descriptor Heap
		ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
		// Constant Buffer View Descriptor Heap
		ComPtr<ID3D12DescriptorHeap> m_cbvHeap;

		// Root Signature:
		// used to define the data that the shaders will access
		ComPtr<ID3D12RootSignature> m_rootSignature;

		D3D12_VIEWPORT m_screenViewport;
		D3D12_RECT m_scissorRect;

		UINT m_rtvDescriptorSize = 0;
		UINT m_dsvDescriptorSize = 0;
		UINT m_cbvSrvUavDescriptorSize = 0;

	public:
		Core();
		Core(const Core& rhs) = delete;
		Core& operator=(const Core& rhs) = delete;
		virtual ~Core();

		void Initialize(HINSTANCE hInstance, HWND hWnd);
		void InitializeDirect3D();
		void OnDestroy();

		void CreateCommandObjects();
		void CreateSwapChain();
		void CreateRtvAndDsvDescriptorHeaps();
		bool CheckTearingSupport() const;

		void BuildDescriptorHeaps();
		void BuildConstantBuffers();
		void BuildRootSignature();

		void PrintAdapterInfo();

		void FlushCommandQueue();

		void Update();
		void ToggleFullscreenWindow(bool fullscreen);
		bool ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
		virtual bool OnResizeWindow(int width, int height);

	public:
		ID3D12Resource* CurrentBackBuffer() const;
		D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView() const;
		D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const;
		FrameResource* CurrentFrameResource() const;
	};
}

