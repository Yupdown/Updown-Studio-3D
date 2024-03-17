#pragma once

#include "pch.h"
#include "component.h"

namespace udsdx
{
	class Scene;
	class Mesh;
	class Shader;

	class MeshRenderer : public Component
	{
	public:
		MeshRenderer(const std::shared_ptr<SceneObject>& object);

	public:
		virtual void Update(const Time& time, Scene& scene) override;
		virtual void Render(ID3D12GraphicsCommandList& cmdl) override;

	public:
		void SetMesh(Mesh* mesh);
		void SetShader(Shader* shader);
		Mesh* GetMesh() const;
		Shader* GetShader() const;

	protected:
		Mesh* m_mesh = nullptr;
		Shader* m_shader = nullptr;
	};
}