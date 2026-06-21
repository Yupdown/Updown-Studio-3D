#include "pch.h"
#include "scene.h"
#include "light_directional.h"
#include "environment_map.h"
#include "shadow_map.h"
#include "screen_space_ao.h"
#include "renderer_base.h"
#include "frame_resource.h"
#include "scene_object.h"
#include "transform.h"
#include "time_measure.h"
#include "camera.h"
#include "shader.h"
#include "core.h"
#include "input.h"
#include "deferred_renderer.h"
#include "motion_blur.h"
#include "post_process_bloom.h"
#include "post_process_taa.h"
#include "post_process_outline.h"
#include "gui_element.h"
#include "debug_console.h"
#include "audio_system.h"

namespace udsdx
{
	namespace
	{
		// Intel TAA (MiniEngine) uses a 16-step Halton(2,3) jitter pattern.
		// 0.5 is neutral, therefore (sample - 0.5) maps to +-0.5 pixel radius.
		constexpr std::array<Vector2, 16> kHalton23_16 = {
			Vector2(0.0f, 0.0f), Vector2(0.5f, 0.333333f), Vector2(0.25f, 0.666667f), Vector2(0.75f, 0.111111f),
			Vector2(0.125f, 0.444444f), Vector2(0.625f, 0.777778f), Vector2(0.375f, 0.222222f), Vector2(0.875f, 0.555556f),
			Vector2(0.0625f, 0.888889f), Vector2(0.5625f, 0.037037f), Vector2(0.3125f, 0.37037f), Vector2(0.8125f, 0.703704f),
			Vector2(0.1875f, 0.148148f), Vector2(0.6875f, 0.481481f), Vector2(0.4375f, 0.814815f), Vector2(0.9375f, 0.259259f)
		};
	}

	extern unsigned long long g_localMatrixRecalculateCounter;
	extern unsigned long long g_worldMatrixRecalculateCounter;

	Scene::Scene()
	{
		m_rootObject = SceneObject::MakeShared();
		m_rootObject->m_sceneRoot = this;
		m_rootObjectSub = SceneObject::MakeShared();
	}

	Scene::~Scene()
	{

	}

	void Scene::OnAttach()
	{
	}

	void Scene::Update(const Time& time)
	{ ZoneScoped;
	    UpdateGUIElementEvent(time);
		SceneObject::Enumerate(m_rootObjectSub, [&time, this](const std::shared_ptr<SceneObject>& sceneObject) { sceneObject->Update(time, *this); });
	}

	void Scene::PostUpdate(const Time& time)
	{
		m_renderCameraQueue.clear();
		m_renderLightQueue.clear();
		m_renderEnvironmentMapQueue.clear();
		for (auto& queue : m_renderObjectQueues)
		{
			queue.clear();
		}
		m_renderShadowObjectQueue.clear();
		m_renderGUIObjectQueue.clear();

		SceneObject::Enumerate(m_rootObjectSub, [&time, this](const std::shared_ptr<SceneObject>& sceneObject) { sceneObject->PostUpdate(time, *this); });
		SceneObject::Enumerate(m_rootObjectSub, [&time, this](const std::shared_ptr<SceneObject>& sceneObject) { sceneObject->GetTransform()->ValidateSRTMatrices(); });
	}

	void Scene::OnDrawGizmos()
	{
		const char* className = typeid(*this).name();

		ImGui::Begin(className);

		unsigned int activeObjectsCount = 0;
		Camera* renderCamera = m_renderCameraQueue[0];
		SceneObject::Enumerate(m_rootObjectSub, [&activeObjectsCount, renderCamera](const std::shared_ptr<SceneObject>& object) {
			activeObjectsCount++;
			object->OnDrawGizmos(renderCamera);
			});
		
		// Add Category for draw calls
		ImGui::SetNextItemOpen(true, ImGuiCond_Once);
		ImGui::Text("Active Objects Count: %u", activeObjectsCount);
		ImGui::Text("Local Matrix Recalculation Count: %zu", g_localMatrixRecalculateCounter);
		ImGui::Text("World Matrix Recalculation Count: %zu", g_worldMatrixRecalculateCounter);
		g_localMatrixRecalculateCounter = 0;
		g_worldMatrixRecalculateCounter = 0;
		if (ImGui::TreeNode("Draw Calls"))
		{
			ImGui::Text("Render Camera Count: %zu", m_renderCameraQueue.size());
			ImGui::Text("Render Light Count: %zu", m_renderLightQueue.size());
			ImGui::Text("Render Shadow Object Count: %zu", m_renderShadowObjectQueue.size());

			for (size_t i = 0; i < m_renderObjectQueues.size(); ++i)
			{
				const auto& group = m_renderObjectQueues[i];
				ImGui::Text("Render Group %zu:", i);
				for (const auto& [defferedPipelineState, objectGroups] : group)
				{
					ImGui::Text("  Deferred Pipeline State: %p", defferedPipelineState);
					for (const auto& [pipelineState, objects] : objectGroups)
					{
						ImGui::Text("    Object Count: %zu", objects.size());
					}
				}
			}
			ImGui::TreePop();
		}

		Matrix4x4 viewMatrix = renderCamera->GetViewMatrix(false);

		Vector3 viewForward = Vector3::TransformNormal(Vector3::UnitZ, viewMatrix);
		Vector3 viewUp = Vector3::TransformNormal(Vector3::UnitY, viewMatrix);
		Vector3 viewRight = viewUp.Cross(viewForward);

		std::array<std::pair<Vector3, ImColor>, 3> lines = {
			std::make_pair(viewRight,	ImColor(1.0f, 0.0f, 0.0f, 1.0f)),		// Red for right
			std::make_pair(viewUp,		ImColor(0.0f, 1.0f, 0.0f, 1.0f)),		// Green for up
			std::make_pair(viewForward, ImColor(0.0f, 0.0f, 1.0f, 1.0f))		// Blue for forward
		};
		std::sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) { return a.first.z > b.first.z; });

		float lineLength = 40.0f;
		float lineThickness = 4.0f;
		ImVec2 screenPosition = ImVec2(ImGui::GetIO().DisplaySize.x - 50.0f, 50.0f);
		ImDrawList* drawList = ImGui::GetBackgroundDrawList();
		for (const auto& line : lines)
		{
			drawList->AddLine(screenPosition,
				screenPosition + ImVec2(line.first.x, -line.first.y) * lineLength,
				line.second, lineThickness);
		}

		const char* text = "View Gizmos";
		ImVec2 textSize = ImGui::CalcTextSize(text);
		drawList->AddText(screenPosition - textSize * 0.5f + ImVec2(0.0f, 50.0f), ImColor(1.0f, 1.0f, 1.0f, 1.0f), text);

		ImGui::End();
	}

	std::vector<D3D12_GPU_VIRTUAL_ADDRESS> Scene::PrepareCameraConstants(RenderParam& param)
	{ ZoneScoped;
		const bool enableTAAJitter = param.RenderOptions->DrawTAA;
		const float viewportWidth = std::max(param.Viewport.Width, 1.0f);
		const float viewportHeight = std::max(param.Viewport.Height, 1.0f);
		const Vector2 haltonSample = kHalton23_16[m_taaFrameIndex % kHalton23_16.size()];
		const Vector2 jitterOffset = enableTAAJitter
			? Vector2(
				(haltonSample.x - 0.5f) / viewportWidth,
				(haltonSample.y - 0.5f) / viewportHeight)
			: Vector2::Zero;

		std::vector<D3D12_GPU_VIRTUAL_ADDRESS> cameraCbvs(m_renderCameraQueue.size());
		for (size_t i = 0; i < m_renderCameraQueue.size(); ++i)
		{
			m_renderCameraQueue[i]->SetClipOffset(jitterOffset);
			cameraCbvs[i] = m_renderCameraQueue[i]->UpdateConstantBuffer(param.FrameResourceIndex, param.Viewport.Width, param.Viewport.Height);
		}
		m_taaFrameIndex++;

		if (!m_renderCameraQueue.empty())
		{
			Transform* listenerTransform = m_renderCameraQueue[0]->GetTransform();
			INSTANCE(AudioSystem)->UpdateAudioListener(listenerTransform->GetWorldPosition(), listenerTransform->GetWorldRotation());
		}

		return cameraCbvs;
	}

	std::vector<ID3D12PipelineState*> Scene::CollectDeferredPipelineStates() const
	{
		std::vector<ID3D12PipelineState*> defferedPipelineStates;
		for (const auto& [defferedPipelineState, objectGroups] : m_renderObjectQueues[RenderGroup::Deferred])
		{
			defferedPipelineStates.push_back(defferedPipelineState);
		}
		return defferedPipelineStates;
	}

	void Scene::OnDetach()
	{
	}

	void Scene::UpdateGUIElementEvent(const Time& time)
	{
		auto elements = m_rootObjectSub->GetComponentsInChildren<GUIElement>();
		int mx = INSTANCE(Input)->GetMouseX();
		int my = INSTANCE(Input)->GetMouseY();

		GUIElement* hoveredElement = nullptr;
		for (auto iter = elements.rbegin(); iter != elements.rend(); ++iter)
		{
			GUIElement* element = *iter;
			if (hoveredElement == nullptr && element->GetRaycastTarget())
			{
				RECT rect = element->GetScreenRect();
				if (rect.left <= mx && mx <= rect.right && rect.top <= my && my <= rect.bottom)
				{
					hoveredElement = element;
				}
			}
			element->UpdateEvent(element == hoveredElement);
		}
	}

	void Scene::AddObject(std::shared_ptr<SceneObject> object)
	{
		m_rootObjectSub->AddChild(object);
	}

	void Scene::HandleAttach()
	{
		OnAttach();

		m_rootObject->AddChild(m_rootObjectSub);
	}

	void Scene::HandleDetach()
	{
		OnDetach();

		m_rootObjectSub->RemoveFromParent();
	}

	void Scene::EnqueueRenderCamera(Camera* camera)
	{
		m_renderCameraQueue.emplace_back(camera);
	}

	void Scene::EnqueueRenderLight(LightDirectional* light)
	{
		m_renderLightQueue.emplace_back(light);
	}

	void Scene::EnqueueRenderEnvironmentMap(EnvironmentMap* environmentMap)
	{
		m_renderEnvironmentMapQueue.emplace_back(environmentMap);
	}

	void Scene::EnqueueRenderObject(RendererBase* object, RenderGroup group, ID3D12PipelineState* pipelineState, ID3D12PipelineState* deferredPipelineState, int parameter)
	{
		m_renderObjectQueues[group][deferredPipelineState][pipelineState].emplace_back(object, parameter);
	}

	void Scene::EnqueueRenderShadowObject(RendererBase* object, ID3D12PipelineState* pipelineState, int parameter)
	{
		m_renderShadowObjectQueue[pipelineState].emplace_back(object, parameter);
	}

	void Scene::EnqueueRenderGUIObject(GUIElement* object)
	{
		m_renderGUIObjectQueue.emplace_back(object);
	}

	void Scene::RenderUI(RenderParam& param)
	{ ZoneScoped;
		// Bind the final output (back buffer) as the render target before drawing the HUD on top of it.
		D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView = param.RenderTargetView;
		param.CommandList->OMSetRenderTargets(1, &renderTargetView, true, nullptr);

		param.SpriteBatchNonPremultipliedAlpha->SetViewport(param.Viewport);
		param.SpriteBatchPreMultipliedAlpha->SetViewport(param.Viewport);
		param.SpriteBatchNonPremultipliedAlpha->Begin(param.CommandList);
		//param.SpriteBatchPreMultipliedAlpha->Begin(param.CommandList);

		RenderGUIObjects(param, 1);

		param.SpriteBatchNonPremultipliedAlpha->End();
		//param.SpriteBatchPreMultipliedAlpha->End();
	}

	void Scene::RenderShadowSceneObjects(RenderParam& param, int instances)
	{
		for (const auto& [pipelineState, objects] : m_renderShadowObjectQueue)
		{
			param.CommandList->SetPipelineState(pipelineState);
			for (const auto& [object, parameter] : objects)
			{
				object->ValidateTransformCache();
				object->Render(param, parameter);
			}
		}
		param.RenderStageIndex++;
	}
	
	void Scene::RenderSceneObjects(RenderParam& param, RenderGroup group, int instances)
	{
		UINT pipelineCount = 0;
		for (const auto& [defferedPipelineState, objectGroups] : m_renderObjectQueues[group])
		{
			if (pipelineCount >= 128)
			{
				DebugConsole::LogWarning("Too many deffered pipeline states in render stage: " + std::to_string(group) + ". Limit is 128.");
				break;
			}

			for (const auto& [pipelineState, objects] : objectGroups)
			{
				param.CommandList->SetPipelineState(pipelineState);

				for (const auto& [object, parameter] : objects)
				{
					param.CommandList->OMSetStencilRef(pipelineCount | (static_cast<UINT>(object->GetDrawOutline()) << 7));
					object->ValidateTransformCache();
					object->Render(param, parameter);
				}
			}

			pipelineCount++;
		}

		param.RenderStageIndex++;
	}

	void Scene::RenderGUIObjects(RenderParam& param, int instances)
	{
		for (const auto& object : m_renderGUIObjectQueue)
		{
			object->Render(param);
		}
	}
}