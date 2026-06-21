#pragma once

#include "pch.h"

namespace udsdx
{
	class SceneObject;
	class RendererBase;
	class GUIElement;
	class Camera;
	class LightDirectional;
	class EnvironmentMap;

	class Scene
	{
	private:
		using RendererGroup = std::unordered_map<ID3D12PipelineState*, std::unordered_map<ID3D12PipelineState*, std::vector<std::pair<RendererBase*, int>>>>;

	public:
		Scene();
		~Scene();

	public:
		// Called when the scene is attached from the core
		virtual void OnAttach();

		// Called every frame
		virtual void Update(const Time& time);

		// Called every frame after Update()
		virtual void PostUpdate(const Time& time);

		// Called when the component needs to draw ImGUI primitives
		virtual void OnDrawGizmos();

		// Called when the scene is detached from the core
		virtual void OnDetach();

		void UpdateGUIElementEvent(const Time& time);
		void AddObject(std::shared_ptr<SceneObject> object);

		void HandleAttach();
		void HandleDetach();

	public:
		void EnqueueRenderCamera(Camera* camera);
		void EnqueueRenderLight(LightDirectional* light);
		void EnqueueRenderEnvironmentMap(EnvironmentMap* environmentMap);
		void EnqueueRenderObject(RendererBase* object, RenderGroup group, ID3D12PipelineState* pipelineState, ID3D12PipelineState* deferredPipelineState, int parameter);
		void EnqueueRenderShadowObject(RendererBase* object, ID3D12PipelineState* pipelineState, int parameter);
		void EnqueueRenderGUIObject(GUIElement* object);

		void RenderShadowSceneObjects(RenderParam& param, int instances = 1);
		void RenderSceneObjects(RenderParam& param, RenderGroup group, int instances = 1);
		void RenderGUIObjects(RenderParam& param, int instances = 1);

	public:
		// Render-queue accessors used by the DeferredRenderer to construct the render passes.
		const std::vector<Camera*>& GetRenderCameras() const { return m_renderCameraQueue; }
		const std::vector<LightDirectional*>& GetRenderLights() const { return m_renderLightQueue; }
		const std::vector<EnvironmentMap*>& GetRenderEnvironmentMaps() const { return m_renderEnvironmentMapQueue; }

		// Collects the distinct deferred (lighting-composition) pipeline states enqueued this frame.
		std::vector<ID3D12PipelineState*> CollectDeferredPipelineStates() const;

		// Per-frame camera constant-buffer / TAA jitter preparation. Returns each camera's CBV GPU address.
		std::vector<D3D12_GPU_VIRTUAL_ADDRESS> PrepareCameraConstants(RenderParam& param);

		// Native UI (HUD / GUI) rendering. Called by the Core after DeferredRenderer::Render.
		void RenderUI(RenderParam& param);

	protected:
		std::shared_ptr<SceneObject> m_rootObject;
		std::shared_ptr<SceneObject> m_rootObjectSub;
		uint64_t m_taaFrameIndex = 0;

		std::vector<Camera*> m_renderCameraQueue;
		std::vector<LightDirectional*> m_renderLightQueue;
		std::vector<EnvironmentMap*> m_renderEnvironmentMapQueue;
		std::array<RendererGroup, 2> m_renderObjectQueues;
		std::unordered_map<ID3D12PipelineState*, std::vector<std::pair<RendererBase*, int>>> m_renderShadowObjectQueue;
		std::vector<GUIElement*> m_renderGUIObjectQueue;
	};
}

