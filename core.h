#pragma once

#include "pch.h"

namespace udsdx
{
	using namespace Microsoft::WRL;

	struct Vertex
	{
		XMFLOAT3 Pos;
		XMFLOAT4 Color;
	};

	struct ObjectConstants
	{
		XMFLOAT4X4 WorldViewProj = MathHelper::Identity4x4();
	};

	class Core
	{
	protected:
		HINSTANCE	m_hInstance = 0;
		HWND		m_hMainWnd = 0;

		std::wstring m_mainWndCaption = L"Updown Studio Application";

		int			m_clientWidth = 800;
		int			m_clientHeight = 600;

		bool		m_appPaused = false;
		bool		m_minimized = false;
		bool		m_maximized = false;
		bool		m_resizing = false;

		// Set true to use 4X MSAA (?.1.8).  The default is false.
		bool		m_4xMsaaState = false;    // 4X MSAA enabled
		UINT		m_4xMsaaQuality = 0;      // quality level of 4X MSAA

		XMFLOAT4X4 m_world = MathHelper::Identity4x4();
		XMFLOAT4X4 m_view = MathHelper::Identity4x4();
		XMFLOAT4X4 m_proj = MathHelper::Identity4x4();

		float m_theta = 1.5f * XM_PI;
		float m_phi = XM_PIDIV4;
		float m_radius = 5.0f;

		std::unique_ptr<Wrappers::RoInitializeWrapper> m_roInitialization;

		// Factory for creating DXGI objects
		ComPtr<IDXGIFactory4> m_dxgiFactory;
		// Direct3D 12 Device
		ComPtr<ID3D12Device> m_d3dDevice;
		// Swap Chain (front and back buffer, similar as double-buffering)
		ComPtr<IDXGISwapChain> m_swapChain;

		ComPtr<ID3D12Fence> m_fence;
		UINT64 m_currentFence = 0;

		ComPtr<ID3D12CommandQueue> m_commandQueue;
		ComPtr<ID3D12CommandAllocator> m_directCommandListAllocator;
		ComPtr<ID3D12GraphicsCommandList> m_commandList;

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
		// Pipeline State Object:
		// used to define the pipeline state (works like a program in OpenGL)
		ComPtr<ID3D12PipelineState> m_pipelineState;

		std::unique_ptr<UploadBuffer<ObjectConstants>> m_objectCB;
		std::unique_ptr<MeshGeometry> m_boxGeometry;

		// Compiled Vertex Shader Code (bytecode)
		ComPtr<ID3DBlob> m_vsByteCode;
		// Compiled Pixel Shader Code (bytecode)
		ComPtr<ID3DBlob> m_psByteCode;

		// Input Layout:
		// used to make vertex propertices associate with the semantics of shaders
		std::vector<D3D12_INPUT_ELEMENT_DESC> m_inputLayout;

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
		void CreateCommandObjects();
		void CreateSwapChain();
		void CreateRtvAndDsvDescriptorHeaps();

		void BuildDescriptorHeaps();
		void BuildConstantBuffers();
		void BuildRootSignature();
		void BuildShadersAndInputLayout();
		void BuildGeometry();
		void BuildPipelineState();

		void FlushCommandQueue();

		void Update();
		bool ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
		virtual void OnResize();

		ID3D12Resource* CurrentBackBuffer() const;
		D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView() const;
		D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const;
	};
}

