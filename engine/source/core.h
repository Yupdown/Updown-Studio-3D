#pragma once

#include "pch.h"

namespace udsdx
{
	class FrameDebug;
	class FrameResource;
	class TimeMeasure;
	class Scene;
	class ShadowMap;
	class ScreenSpaceAO;
	class DeferredRenderer;
	class MotionBlur;
	class PostProcessBloom;
	class PostProcessTAA;
	class PostProcessOutline;
	class Texture;

	class Core
	{
	public:
		Core();
		Core(const Core& rhs) = delete;
		Core& operator=(const Core& rhs) = delete;
		virtual ~Core();

		void Initialize(HINSTANCE hInstance, HWND hWnd);

		void DisplayFrameStats();
		void OnDestroy();

		void RegisterDescriptorsToHeaps();
		void BuildConstantBuffers();

		void ExecuteCommandList();
		void FlushCommandQueue();

		void PrepareDirectCommandList();
		void ExecuteAndFlushDirectCommandList();

		void SetScene(std::shared_ptr<Scene> scene);
		void RegisterUpdateCallback(std::function<void(const Time&)> callback);
		void AcquireNextFrameResource();
		void Update();
		void BroadcastUpdateMessage();
		void Render();
		void UpdateMainPassCB();
		void SetWindowFullscreen(bool fullscreen);
		LRESULT ProcessMessage(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
		virtual bool OnResizeWindow(int width, int height);

		void InitImGui();
		void ImGuiNewFrame();
		void ImGuiRender();
		void ReleaseImGui();

	public:
		HINSTANCE GetInstance() const;
		HWND GetMainWindow() const;
		ID3D12Device* GetDevice() const;
		ID3D12Device2* GetDevice2() const;
		ID3D12CommandQueue* GetCommandQueue() const;
		ID3D12CommandAllocator* GetCommandAllocator() const;
		ID3D12GraphicsCommandList2* GetCommandList() const;
		bool IsShadowViewInstancingSupported() const;
		ID3D12DescriptorHeap* GetSrvDescriptorHeap() const;
		DeferredRenderer* GetRenderer() const;
		ShadowMap* GetShadowMap() const;
		ScreenSpaceAO* GetScreenSpaceAO() const;
		MonoUploadBuffer* GetMonoUploadBuffer() const;

		FrameResource* CurrentFrameResource() const;
		ID3D12Resource* CurrentBackBuffer() const;

		D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView() const;

		DescriptorParam GetDescriptorParameters() const;
		void ApplyDescriptorParameters(const DescriptorParam& param);
		void EnsureTextureShaderResourceView(Texture* texture);
		RenderOptions& GetRenderOptionsRef();

		int GetClientPosX() const;
		int GetClientPosY() const;
		int GetClientWidth() const;
		int GetClientHeight() const;
		float GetAspectRatio() const;

	protected:
		// Initialize DXGI Factory, Direct3D 12 Device, etc.
		void InitializeDirect3D();

		void EnableDebugLayer();

		// Create Direct3D 12 Command Queue, Command List, Command Allocator
		void CreateCommandObjects();

		// Create Direct3D 12 Swap Chain using DXGI Factory
		void CreateSwapChain();

		// Create Direct3D 12 Descriptor Heaps for
		// * Constant Buffer View (CBV)
		// * Shader Resource View (SRV)
		// * Render Target View (RTV)
		// * Depth/Stencil View (DSV)
		void CreateDescriptorHeaps();

		// Enumerate adapters and outputs using DXGI Factory
		void LogAdapterInfo();

		void InitializeSpriteBatch();

	protected:
		HINSTANCE	m_hInstance = 0;
		HWND		m_hMainWnd = 0;

		std::wstring m_mainWndCaption;

		int			m_clientPosX = 100;
		int			m_clientPosY = 100;
		int			m_clientWidth = 800;
		int			m_clientHeight = 600;

		int			m_minClientWidth = 240;
		int 	    m_minClientHeight = 240;

		bool		m_fullscreen = false;

		RECT		m_windowedRect;

		BOOL		m_tearingSupport = false;

		// Set true to use 4X MSAA (?.1.8).  The default is false.
		bool		m_4xMsaaState = false;    // 4X MSAA enabled
		UINT		m_4xMsaaQuality = 0;      // quality level of 4X MSAA

		bool		m_drawImGUIElements = false; // Draw ImGui elements on the screen

		// Current Scene to render with
		std::shared_ptr<Scene> m_scene;
		std::function<void(const Time&)> m_updateCallback = nullptr;

	protected:
		TimeMeasure* m_timeMeasure;
		std::unique_ptr<Wrappers::RoInitializeWrapper> m_roInitialization;

		// Factory for creating DXGI objects
		// For enumerating adapters, monitors, video modes, etc. and tp create swap chains
		// Has versions for compability with newer versions of OS (inherited from each earlier version)
		// IDXGIFactory[N] where N is the version number
		// 
		// DXGI Objects
		// * DXGI Adapter: represents a display subsystem (including one or more GPUs)
		// * DXGI Output: represents an output on an adapter (monitor)
		ComPtr<IDXGIFactory6> m_dxgiFactory;

		// Direct3D 12 Device
		ComPtr<ID3D12Device> m_d3dDevice;
		// Same device, queried to ID3D12Device2 for CreatePipelineState (pipeline state stream)
		ComPtr<ID3D12Device2> m_d3dDevice2;

		// View instancing support for the cascaded shadow map pass:
		// requires OPTIONS3::ViewInstancingTier >= 1 and shader model 6.1 (SV_ViewID)
		D3D12_VIEW_INSTANCING_TIER m_viewInstancingTier = D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
		bool m_shadowViewInstancingSupported = false;

		// Swap Chain (front and back buffer, similar as double-buffering)
		ComPtr<IDXGISwapChain4> m_swapChain;

		// Fence for CPU/GPU synchronization
		ComPtr<ID3D12Fence> m_fence;
		UINT64 m_currentFence = 0;
		HANDLE m_fenceEvent = nullptr;

		// There are three types of command queues: Copy Queue, Rendering Queue, Compute Queue
		// But Rendering Queue can use all types of engines (Copy, Compute, Direct)
		// therefore, you can use only Rendering Queue for every purpose
		ComPtr<ID3D12CommandQueue> m_commandQueue;

		// Allocating memory space for commands
		// Has Open / Close states to record commands
		// Can not be opened simultaneously by multiple command lists
		// This allocator is used for the initialization (not for rendering)
		ComPtr<ID3D12CommandAllocator> m_directCmdListAlloc;

		// A collection of commands to be appended to a command queue
		// (ID3D12GraphicsCommandList2 for SetViewInstanceMask)
		ComPtr<ID3D12GraphicsCommandList2> m_commandList;

		// Frame Resources for parameters of each frame
		// Each frame resource contains a command allocator and
		// * Constant Buffer (Per each global pass)
		// * Render Target
		std::array<std::unique_ptr<FrameResource>, FrameResourceCount> m_frameResources;
		int m_currFrameResourceIndex = 0;

		// Swap Chain Buffers
		// Prepared for the next frame and presented to the screen
		std::array<ComPtr<ID3D12Resource>, SwapChainBufferCount> m_swapChainBuffers;
		int m_currBackBuffer = 0;

		// Deferred Renderer (owns the render passes, depth buffer, render options, and root signatures)
		std::unique_ptr<DeferredRenderer> m_deferredRenderer;

		// Descriptor Heap
		// A continuous block of memory containing descriptors which describe resources
		// There are 4 types of descriptor heaps: CBV, SRV, UAV, Sampler	-> D3D12_DESCRIPTOR_HEAP_TYPE
		// UINT NumDescriptors: the number of descriptors in the heap

		// Render Target View Descriptor Heap
		ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
		// Depth/Stencil View Descriptor Heap
		ComPtr<ID3D12DescriptorHeap> m_dsvHeap;
		// Constant Buffer View Descriptor Heap
		ComPtr<ID3D12DescriptorHeap> m_cbvHeap;
		// Shader Resource View Descriptor Heap
		ComPtr<ID3D12DescriptorHeap> m_srvHeap;

		D3D12_VIEWPORT m_screenViewport;
		D3D12_RECT m_scissorRect;

		UINT m_rtvDescriptorSize = 0;
		UINT m_dsvDescriptorSize = 0;
		UINT m_cbvSrvUavDescriptorSize = 0;

		UINT m_cbvHeapSize = 0;
		UINT m_srvHeapSize = 0;
		UINT m_rtvHeapSize = SwapChainBufferCount;
		UINT m_dsvHeapSize = 0;

		std::unique_ptr<FrameDebug> m_frameDebug;
		TracyD3D12Ctx m_tracyQueueCtx;

		std::unique_ptr<GraphicsMemory> m_graphicsMemory;
		std::unique_ptr<MonoUploadBuffer> m_monoUploadBuffer;

		// DirectXTK Sprite Batch for HUD rendering
		std::unique_ptr<SpriteBatch> m_hudSpriteBatch;
		std::unique_ptr<SpriteBatch> m_hudSpriteBatchPremultipliedAlpha;
	};
}

