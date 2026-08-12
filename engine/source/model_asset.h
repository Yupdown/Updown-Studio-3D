#pragma once

#include "pch.h"
#include "resource_object.h"
#include "material.h"

struct aiScene;
struct aiNode;

namespace udsdx
{
	class MeshBase;
	class Texture;
	class Shader;
	class AnimationClip;
	class SceneObject;

	// A model file (fbx/obj/glb/gltf/dae) loaded as a glTF-style document: an internal scene graph
	// of nodes referencing meshes, meshes referencing materials, materials referencing textures.
	// The asset owns its meshes, materials, and embedded textures; external image textures stay
	// owned by the Resource manager and are merely referenced. Instantiate() spawns a SceneObject
	// hierarchy mirroring the graph, with a user-supplied shader injected into the materials.
	class ModelAsset : public ResourceObject
	{
	public:
		struct Node
		{
			std::string Name;
			Matrix4x4 LocalTransform = Matrix4x4::Identity;
			std::vector<int> Children;   // indices into m_nodes
			std::vector<int> MeshIndices; // indices into m_meshes
			int Parent = -1;
		};

	public:
		ModelAsset(const aiScene* scene, const std::filesystem::path& resourcePath, ID3D12Device* device, ID3D12GraphicsCommandList* commandList);
		~ModelAsset();

		// Builds a SceneObject hierarchy mirroring the node graph, each object named after its node.
		// Static assets attach a MeshRenderer to nodes that reference meshes. Rigged assets spawn
		// the skeleton as those named SceneObjects and attach to the root a RiggedMeshRenderer
		// (skinning from the bone objects' Transforms) plus, when the asset has animations, an
		// Animator registered with every clip, auto-playing the first one looped. Materials get
		// 'shader' injected. Returns the root SceneObject (the caller adds it to a Scene), or
		// nullptr if the asset is empty.
		// Pass enableRaytracing to attach RaytracingMeshRenderer instead of MeshRenderer on static
		// nodes, so the instantiated geometry joins the raytracing acceleration structure. Rigged
		// assets are unaffected: skinned meshes have no GPU vertex buffer to build a BLAS from.
		std::shared_ptr<SceneObject> Instantiate(Shader* shader, bool enableRaytracing = false) const;

		bool IsRigged() const { return m_isRigged; }
		const std::vector<Node>& GetNodes() const { return m_nodes; }
		const std::vector<int>& GetRootNodes() const { return m_rootNodes; }
		size_t GetMeshCount() const { return m_meshes.size(); }
		MeshBase* GetMesh(size_t index) const;
		size_t GetMaterialCount() const { return m_materials.size(); }
		Material* GetMaterial(size_t index) const;
		// Animation clip identified by its key (the source animation's name), or nullptr if absent.
		// Each animation in the source model is its own clip.
		const AnimationClip* GetAnimationClip(std::string_view key) const;
		// Keys of all animation clips, in source-scene order.
		std::vector<std::string> GetAnimationClipNames() const;

	private:
		// Encoded bytes of one compressed texture embedded in the model, staged until
		// ResolveEmbeddedTextures() turns it into m_embeddedTextures[i].
		struct EmbeddedTextureBlob
		{
			std::string Name;
			std::wstring FormatHint;
			std::vector<uint8_t> Data;
			bool IsHdr = false;
		};
		// A material slot waiting on an embedded texture that does not exist until resolve time.
		struct PendingEmbedded
		{
			int MaterialIndex;
			MaterialTextureSlot Slot;
			int EmbeddedIndex;
		};

		int BuildNodeGraph(const aiScene* scene, const aiNode* node, int parent);
		void BuildMaterials(const aiScene* scene, const std::filesystem::path& resourcePath);
		void BuildStatic(const aiScene* scene);
		void BuildRigged(const aiScene* scene);
		// Builds one AnimationClip per source animation, keyed by the animation's name.
		void BuildAnimations(const aiScene* scene);
		void ExtractMaterialTextures(const aiScene* scene, unsigned int materialIndex, Material& material, const std::filesystem::path& resourcePath);
		int RegisterEmbeddedTexture(const aiScene* scene, const char* assimpPath);
		void ResolveEmbeddedTextures(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);
		std::shared_ptr<SceneObject> InstantiateNode(int nodeIndex, Shader* shader, bool enableRaytracing) const;
		// Resource-owned material for a submesh, or the default material when the index is absent.
		Material* MaterialForSubmesh(int materialIndex) const;

	private:
		bool m_isRigged = false;

		std::vector<Node> m_nodes;
		std::vector<int> m_rootNodes;

		std::vector<std::unique_ptr<MeshBase>> m_meshes;
		// Material index per submesh, indexed [meshIndex][submeshIndex]. Static meshes have one
		// submesh; the single rigged mesh has one entry per skinned submesh.
		std::vector<std::vector<int>> m_meshSubmeshMaterials;

		// Non-owning: Resource owns every material. They outlive this asset, which is harmless
		// today because nothing unloads resources individually -- revisit if that changes.
		std::vector<Material*> m_materials;

		std::vector<std::unique_ptr<Texture>> m_embeddedTextures;
		std::vector<EmbeddedTextureBlob> m_embeddedTextureBlobs;
		std::unordered_map<int, int> m_embeddedIndexMap;
		std::vector<PendingEmbedded> m_pendingEmbedded;

		// Animation clips owned in source-scene order, plus a key (name) -> clip lookup.
		std::vector<std::unique_ptr<AnimationClip>> m_animationClips;
		std::unordered_map<std::string, AnimationClip*> m_animationClipMap;
	};
}
