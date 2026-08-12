#include "pch.h"
#include "model_asset.h"
#include "mesh.h"
#include "rigged_mesh.h"
#include "texture.h"
#include "shader.h"
#include "scene_object.h"
#include "transform.h"
#include "mesh_renderer.h"
#include "raytracing_mesh_renderer.h"
#include "rigged_mesh_renderer.h"
#include "animation_clip.h"
#include "animator.h"
#include "core.h"
#include "resource_load.h"
#include "debug_console.h"
#include <assimp/scene.h>
#include <assimp/material.h>
// AI_MATKEY_GLTF_ALPHAMODE / _ALPHACUTOFF and the per-texture scale/strength keys live here.
#include <assimp/GltfMaterial.h>

namespace udsdx
{
	namespace
	{
		// Scene-level rigged detection: the whole asset is rigged if the scene carries animations or
		// any mesh has bones.
		bool SceneIsRigged(const aiScene* scene)
		{
			if (scene->mNumAnimations > 0)
			{
				return true;
			}
			for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
			{
				if (scene->mMeshes[i]->HasBones())
				{
					return true;
				}
			}
			return false;
		}
	}

	ModelAsset::ModelAsset(const aiScene* scene, const std::filesystem::path& resourcePath, ID3D12Device* device, ID3D12GraphicsCommandList* commandList) : ResourceObject(resourcePath.wstring())
	{
		m_isRigged = SceneIsRigged(scene);

		BuildMaterials(scene, resourcePath);

		if (m_isRigged)
		{
			BuildRigged(scene);
		}
		else
		{
			BuildStatic(scene);
		}

		BuildAnimations(scene);

		// Build the GPU textures for embedded images, then wire them into the materials that
		// referenced them (the Texture* did not exist when the material was first parsed).
		ResolveEmbeddedTextures(device, commandList);
		for (const PendingEmbedded& pending : m_pendingEmbedded)
		{
			if (pending.EmbeddedIndex >= 0 && static_cast<size_t>(pending.EmbeddedIndex) < m_embeddedTextures.size()
				&& static_cast<size_t>(pending.MaterialIndex) < m_materials.size())
			{
				m_materials[pending.MaterialIndex]->SetSourceTexture(m_embeddedTextures[pending.EmbeddedIndex].get(), pending.Slot);
			}
		}

		for (auto& mesh : m_meshes)
		{
			mesh->UploadBuffers(device, commandList);
		}
	}

	ModelAsset::~ModelAsset() = default;

	MeshBase* ModelAsset::GetMesh(size_t index) const
	{
		return index < m_meshes.size() ? m_meshes[index].get() : nullptr;
	}

	Material* ModelAsset::GetMaterial(size_t index) const
	{
		return index < m_materials.size() ? m_materials[index] : nullptr;
	}

	const AnimationClip* ModelAsset::GetAnimationClip(std::string_view key) const
	{
		auto iter = m_animationClipMap.find(std::string(key));
		return iter != m_animationClipMap.end() ? iter->second : nullptr;
	}

	std::vector<std::string> ModelAsset::GetAnimationClipNames() const
	{
		std::vector<std::string> names;
		names.reserve(m_animationClips.size());
		for (const auto& clip : m_animationClips)
		{
			names.push_back(clip->GetName().data());
		}
		return names;
	}

	int ModelAsset::BuildNodeGraph(const aiScene* scene, const aiNode* node, int parent)
	{
		const int index = static_cast<int>(m_nodes.size());
		m_nodes.emplace_back(); // reserve this slot; filled after children are appended

		Node nodeData;
		nodeData.Name = node->mName.C_Str();
		nodeData.Parent = parent;

		// Same Assimp->DirectXMath convention used when baking transforms previously: transpose the
		// row-major aiMatrix and load it as a row-vector matrix. Kept LOCAL (not composed) so the
		// instantiated SceneObject hierarchy reproduces the original world transforms.
		aiMatrix4x4 transposed = node->mTransformation;
		transposed.Transpose();
		XMStoreFloat4x4(&nodeData.LocalTransform, XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(&transposed.a1)));

		for (unsigned int i = 0; i < node->mNumMeshes; ++i)
		{
			nodeData.MeshIndices.push_back(static_cast<int>(node->mMeshes[i]));
		}

		for (unsigned int i = 0; i < node->mNumChildren; ++i)
		{
			const int childIndex = BuildNodeGraph(scene, node->mChildren[i], index);
			nodeData.Children.push_back(childIndex);
		}

		m_nodes[index] = std::move(nodeData);
		return index;
	}

	void ModelAsset::BuildStatic(const aiScene* scene)
	{
		m_meshes.reserve(scene->mNumMeshes);
		m_meshSubmeshMaterials.reserve(scene->mNumMeshes);
		for (unsigned int i = 0; i < scene->mNumMeshes; ++i)
		{
			const aiMesh* aimesh = scene->mMeshes[i];
			m_meshes.push_back(std::make_unique<Mesh>(scene, aimesh));
			m_meshSubmeshMaterials.push_back({ static_cast<int>(aimesh->mMaterialIndex) });
		}

		m_rootNodes.push_back(BuildNodeGraph(scene, scene->mRootNode, -1));
	}

	void ModelAsset::BuildRigged(const aiScene* scene)
	{
		// One RiggedMesh owns the whole skeleton, every skinned submesh, and the embedded clip.
		auto rigged = std::make_unique<RiggedMesh>(scene);

		std::vector<int> submeshMaterials;
		const std::vector<unsigned int>& materialIndices = rigged->GetSubmeshMaterialIndices();
		submeshMaterials.reserve(materialIndices.size());
		for (unsigned int materialIndex : materialIndices)
		{
			submeshMaterials.push_back(static_cast<int>(materialIndex));
		}
		m_meshSubmeshMaterials.push_back(std::move(submeshMaterials));
		m_meshes.push_back(std::move(rigged));

		m_rootNodes.push_back(BuildNodeGraph(scene, scene->mRootNode, -1));

		// Node MeshIndices reference source-scene meshes, which do not exist for the merged
		// RiggedMesh; the renderer attaches to the root at Instantiate time instead.
		for (Node& node : m_nodes)
		{
			node.MeshIndices.clear();
		}
	}

	void ModelAsset::BuildAnimations(const aiScene* scene)
	{
		// Each source animation becomes its own AnimationClip (skeleton + that one animation), looked
		// up by the animation's name.
		m_animationClips.reserve(scene->mNumAnimations);
		for (unsigned int i = 0; i < scene->mNumAnimations; ++i)
		{
			const aiAnimation* animationSrc = scene->mAnimations[i];
			auto clip = std::make_unique<AnimationClip>(scene, animationSrc);
			m_animationClipMap.emplace(animationSrc->mName.C_Str(), clip.get());
			m_animationClips.push_back(std::move(clip));
		}
	}

	void ModelAsset::BuildMaterials(const aiScene* scene, const std::filesystem::path& resourcePath)
	{
		// Resource owns the materials; this asset only keeps pointers. Keying them off the asset
		// path keeps two models from colliding, and re-loading the same model reuses them.
		const std::wstring assetKey = resourcePath.wstring();
		m_materials.reserve(scene->mNumMaterials);
		for (unsigned int i = 0; i < scene->mNumMaterials; ++i)
		{
			Material* material = INSTANCE(Resource)->CreateMaterial(assetKey + L"|mat" + std::to_wstring(i));
			ExtractMaterialTextures(scene, i, *material, resourcePath);
			m_materials.push_back(material);
		}
	}

	void ModelAsset::ExtractMaterialTextures(const aiScene* scene, unsigned int materialIndex, Material& material, const std::filesystem::path& resourcePath)
	{
		const aiMaterial* aimaterial = scene->mMaterials[materialIndex];
		if (aimaterial == nullptr)
		{
			return;
		}

		ReadMaterialScalars(aimaterial, material);

		// Fallback chains, first hit wins. glTF2 registers several of these under more than one
		// type (base colour under both DIFFUSE and BASE_COLOR, metallic-roughness under
		// METALNESS, DIFFUSE_ROUGHNESS and GLTF_METALLIC_ROUGHNESS), and the trailing entries
		// cover the conventions other importers use.
		struct Slot { MaterialTextureSlot Index; aiTextureType Types[3]; };
		const Slot slots[] = {
			{ MaterialTextureSlot::BaseColor,
				{ aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE, aiTextureType_NONE } },
			{ MaterialTextureSlot::MetallicRoughness,
				{ aiTextureType_GLTF_METALLIC_ROUGHNESS, aiTextureType_METALNESS, aiTextureType_DIFFUSE_ROUGHNESS } },
			{ MaterialTextureSlot::Normal,
				// FBX exporters routinely park the normal map in HEIGHT.
				{ aiTextureType_NORMALS, aiTextureType_HEIGHT, aiTextureType_NONE } },
			{ MaterialTextureSlot::Occlusion,
				// glTF2 maps occlusionTexture to LIGHTMAP, never to AMBIENT_OCCLUSION.
				{ aiTextureType_LIGHTMAP, aiTextureType_AMBIENT_OCCLUSION, aiTextureType_NONE } },
			{ MaterialTextureSlot::Emissive,
				{ aiTextureType_EMISSIVE, aiTextureType_EMISSION_COLOR, aiTextureType_NONE } },
		};

		std::array<std::string, NumMaterialTextureSlots> slotPaths;

		for (const Slot& slot : slots)
		{
			aiString texturePath;
			aiTextureType foundType = aiTextureType_NONE;
			for (aiTextureType type : slot.Types)
			{
				if (type != aiTextureType_NONE && aimaterial->GetTexture(type, 0, &texturePath) == AI_SUCCESS)
				{
					foundType = type;
					break;
				}
			}
			if (foundType == aiTextureType_NONE)
			{
				continue;
			}

			WarnUnsupportedTextureMapping(aimaterial, foundType, material);

			const char* raw = texturePath.C_Str();
			slotPaths[static_cast<size_t>(slot.Index)] = raw != nullptr ? raw : "";
			if (raw != nullptr && raw[0] == '*')
			{
				// Embedded texture ("*N"): defer wiring until the Texture exists (resolve time).
				const int embeddedIndex = RegisterEmbeddedTexture(scene, raw);
				if (embeddedIndex >= 0)
				{
					m_pendingEmbedded.push_back({ static_cast<int>(materialIndex), slot.Index, embeddedIndex });
					continue;
				}
			}

			// External image: resolve against the model file's directory and load it as a normal
			// Resource-owned texture. Skip silently when the file is missing (the caller may
			// override the material) so a missing sidecar never aborts the whole asset load.
			std::filesystem::path resolved = resourcePath.parent_path() / std::filesystem::path(raw);
			if (std::filesystem::exists(resolved))
			{
				Texture* texture = INSTANCE(Resource)->Load<Texture>(Resource::NormalizePath(resolved.wstring()));
				if (texture != nullptr)
				{
					material.SetSourceTexture(texture, slot.Index);
				}
			}
		}

		// glTF packs occlusion/roughness/metallic into one image and references it from both
		// occlusionTexture and metallicRoughnessTexture. Assimp surfaces them under unrelated
		// texture types, so comparing the referenced paths is the only way to notice. Shading does
		// not depend on spotting it -- both slots resolve to the same Texture and each channel is
		// read where glTF says it lives -- but a packed set is worth knowing about.
		const std::string& occlusionPath = slotPaths[static_cast<size_t>(MaterialTextureSlot::Occlusion)];
		const std::string& metalRoughPath = slotPaths[static_cast<size_t>(MaterialTextureSlot::MetallicRoughness)];
		if (!occlusionPath.empty() && occlusionPath == metalRoughPath)
		{
			material.SetOrmPacked(true);
		}
	}

	void ModelAsset::ReadMaterialScalars(const aiMaterial* aimaterial, Material& material)
	{
		aiColor4D color4;
		// glTF2 writes baseColorFactor to both keys; other importers only fill COLOR_DIFFUSE.
		if (aimaterial->Get(AI_MATKEY_BASE_COLOR, color4) == AI_SUCCESS ||
			aimaterial->Get(AI_MATKEY_COLOR_DIFFUSE, color4) == AI_SUCCESS)
		{
			material.SetBaseColorFactor(Color(color4.r, color4.g, color4.b, color4.a));
		}

		aiColor3D color3;
		if (aimaterial->Get(AI_MATKEY_COLOR_EMISSIVE, color3) == AI_SUCCESS)
		{
			material.SetEmissiveFactor(Vector3(color3.r, color3.g, color3.b));
		}

		float value = 0.0f;
		// Left at the engine defaults when absent. In particular metallic stays 0 rather than
		// glTF's 1: the key is always present for glTF, so only non-glTF assets see the default.
		if (aimaterial->Get(AI_MATKEY_METALLIC_FACTOR, value) == AI_SUCCESS)
		{
			material.SetMetallicFactor(value);
		}
		if (aimaterial->Get(AI_MATKEY_ROUGHNESS_FACTOR, value) == AI_SUCCESS)
		{
			material.SetRoughnessFactor(value);
		}
		if (aimaterial->Get(AI_MATKEY_EMISSIVE_INTENSITY, value) == AI_SUCCESS)
		{
			material.SetEmissiveStrength(value);
		}
		// Per-texture scalars glTF hangs off the texture reference rather than the material.
		if (aimaterial->Get(AI_MATKEY_GLTF_TEXTURE_SCALE(aiTextureType_NORMALS, 0), value) == AI_SUCCESS)
		{
			material.SetNormalScale(value);
		}
		if (aimaterial->Get(AI_MATKEY_GLTF_TEXTURE_STRENGTH(aiTextureType_LIGHTMAP, 0), value) == AI_SUCCESS)
		{
			material.SetOcclusionStrength(value);
		}
		if (aimaterial->Get(AI_MATKEY_GLTF_ALPHACUTOFF, value) == AI_SUCCESS)
		{
			material.SetAlphaCutoff(value);
		}
		// Only meaningful with KHR_materials_ior; other importers write their own generic default.
		if (aimaterial->Get(AI_MATKEY_REFRACTI, value) == AI_SUCCESS)
		{
			material.SetIor(value);
		}

		int flag = 0;
		if (aimaterial->Get(AI_MATKEY_TWOSIDED, flag) == AI_SUCCESS)
		{
			material.SetDoubleSided(flag != 0);
		}

		aiString alphaMode;
		if (aimaterial->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS)
		{
			const std::string mode = alphaMode.C_Str();
			if (mode == "MASK")
			{
				material.SetAlphaMode(MaterialAlphaMode::Mask);
			}
			else if (mode == "BLEND")
			{
				material.SetAlphaMode(MaterialAlphaMode::Blend);
			}
			else
			{
				material.SetAlphaMode(MaterialAlphaMode::Opaque);
			}
		}
		else if (aimaterial->Get(AI_MATKEY_OPACITY, value) == AI_SUCCESS && value < 1.0f)
		{
			// No glTF alpha mode: fall back to the generic opacity every other importer writes, so
			// a translucent FBX/OBJ material is not silently promoted to opaque.
			material.SetAlphaMode(MaterialAlphaMode::Blend);
		}
	}

	void ModelAsset::WarnUnsupportedTextureMapping(const aiMaterial* aimaterial, aiTextureType type, const Material& material)
	{
		// The engine carries one UV set and applies no texture transform. Both are expressible in
		// glTF, so say so out loud rather than rendering something subtly wrong in silence.
		int uvIndex = 0;
		if (aimaterial->Get(AI_MATKEY_UVWSRC(type, 0), uvIndex) == AI_SUCCESS && uvIndex != 0)
		{
			DebugConsole::LogWarning("Material references TEXCOORD_" + std::to_string(uvIndex) +
				"; the engine has a single UV set, so UV0 is used instead.");
		}

		aiUVTransform transform;
		if (aimaterial->Get(AI_MATKEY_UVTRANSFORM(type, 0), transform) == AI_SUCCESS)
		{
			const bool identity = transform.mTranslation.x == 0.0f && transform.mTranslation.y == 0.0f
				&& transform.mScaling.x == 1.0f && transform.mScaling.y == 1.0f
				&& transform.mRotation == 0.0f;
			if (!identity)
			{
				DebugConsole::LogWarning("Material carries a KHR_texture_transform; it is not applied.");
			}
		}
	}

	int ModelAsset::RegisterEmbeddedTexture(const aiScene* scene, const char* assimpPath)
	{
		const aiTexture* embedded = scene->GetEmbeddedTexture(assimpPath);
		if (embedded == nullptr)
		{
			return -1;
		}

		// Only compressed embedded textures are supported: Assimp signals these with mHeight == 0,
		// where pcData holds the raw encoded file bytes and achFormatHint the extension.
		if (embedded->mHeight != 0)
		{
			DebugConsole::LogWarning("Skipping uncompressed (raw pixel) embedded texture; only compressed embedded textures are supported.");
			return -1;
		}

		const int sceneIndex = std::atoi(assimpPath + 1);
		auto iter = m_embeddedIndexMap.find(sceneIndex);
		if (iter != m_embeddedIndexMap.end())
		{
			return iter->second;
		}

		EmbeddedTextureBlob blob;
		blob.Name = (embedded->mFilename.length > 0) ? embedded->mFilename.C_Str() : ("embedded_" + std::to_string(sceneIndex));

		std::string hint = embedded->achFormatHint;
		blob.FormatHint = std::wstring(hint.begin(), hint.end());
		blob.IsHdr = (hint == "hdr");

		const size_t byteSize = embedded->mWidth; // for compressed textures mWidth is the byte count
		const uint8_t* bytes = reinterpret_cast<const uint8_t*>(embedded->pcData);
		blob.Data.assign(bytes, bytes + byteSize);

		const int listIndex = static_cast<int>(m_embeddedTextureBlobs.size());
		m_embeddedTextureBlobs.emplace_back(std::move(blob));
		m_embeddedIndexMap.emplace(sceneIndex, listIndex);
		return listIndex;
	}

	void ModelAsset::ResolveEmbeddedTextures(ID3D12Device* device, ID3D12GraphicsCommandList* commandList)
	{
		m_embeddedTextures.clear();
		m_embeddedTextures.reserve(m_embeddedTextureBlobs.size());

		for (size_t i = 0; i < m_embeddedTextureBlobs.size(); ++i)
		{
			const EmbeddedTextureBlob& blob = m_embeddedTextureBlobs[i];

			std::wstring key = L"embedded:" + std::wstring(blob.Name.begin(), blob.Name.end()) + L"*" + std::to_wstring(i);
			auto texture = std::make_unique<Texture>(
				key, blob.Name, blob.Data.data(), blob.Data.size(), blob.FormatHint, blob.IsHdr, device, commandList);
			INSTANCE(Core)->EnsureTextureShaderResourceView(texture.get());
			m_embeddedTextures.emplace_back(std::move(texture));
		}

		// The staged bytes are no longer needed once the GPU textures exist.
		m_embeddedTextureBlobs.clear();
		m_embeddedTextureBlobs.shrink_to_fit();
	}

	Material* ModelAsset::MaterialForSubmesh(int materialIndex) const
	{
		if (materialIndex >= 0 && static_cast<size_t>(materialIndex) < m_materials.size())
		{
			return m_materials[materialIndex];
		}
		return INSTANCE(Resource)->GetDefaultMaterial();
	}

	std::shared_ptr<SceneObject> ModelAsset::Instantiate(Shader* shader, bool enableRaytracing) const
	{
		if (m_rootNodes.empty())
		{
			return nullptr;
		}

		std::shared_ptr<SceneObject> root;
		if (m_rootNodes.size() == 1)
		{
			root = InstantiateNode(m_rootNodes[0], shader, enableRaytracing);
		}
		else
		{
			// Multiple roots: wrap them under a single empty parent so the caller gets one object.
			root = SceneObject::MakeShared();
			for (int rootNode : m_rootNodes)
			{
				root->AddChild(InstantiateNode(rootNode, shader, enableRaytracing));
			}
		}

		// The skeleton now exists as the named child SceneObjects built above, so the renderer can
		// resolve its bones; the Animator drives them by writing local transforms each frame.
		if (m_isRigged && !m_meshes.empty())
		{
			auto renderer = root->AddComponent<RiggedMeshRenderer>();
			renderer->SetMesh(static_cast<RiggedMesh*>(m_meshes[0].get()));
			renderer->SetShader(shader);
			const std::vector<int>& submeshMaterials = m_meshSubmeshMaterials[0];
			for (size_t submeshIndex = 0; submeshIndex < submeshMaterials.size(); ++submeshIndex)
			{
				renderer->SetMaterial(MaterialForSubmesh(submeshMaterials[submeshIndex]), static_cast<int>(submeshIndex));
			}
			renderer->RebindBones();

			// Auto-play the first animation (looping) so a rigged asset animates out of the box;
			// the caller can switch via animator->Play(key, ...).
			if (!m_animationClips.empty())
			{
				auto animator = root->AddComponent<Animator>();
				for (const auto& clip : m_animationClips)
				{
					animator->AddClip(clip.get());
				}
				animator->Play(m_animationClips.front().get(), true);
			}
		}

		return root;
	}

	std::shared_ptr<SceneObject> ModelAsset::InstantiateNode(int nodeIndex, Shader* shader, bool enableRaytracing) const
	{
		const Node& node = m_nodes[nodeIndex];
		auto object = SceneObject::MakeShared();
		object->SetName(node.Name);

		// Transform has no set-from-matrix path; decompose the node's local matrix into TRS.
		Vector3 scale;
		Quaternion rotation;
		Vector3 translation;
		Matrix4x4 localTransform = node.LocalTransform;
		localTransform.Decompose(scale, rotation, translation);
		object->GetTransform()->SetLocalPosition(translation);
		object->GetTransform()->SetLocalRotation(rotation);
		object->GetTransform()->SetLocalScale(scale);

		// Rigged nodes carry no MeshIndices: the merged RiggedMesh renders from the root object,
		// attached by Instantiate() once the whole hierarchy exists.
		if (!m_isRigged)
		{
			for (int meshIndex : node.MeshIndices)
			{
				if (meshIndex < 0 || static_cast<size_t>(meshIndex) >= m_meshes.size())
				{
					continue;
				}
				const std::vector<int>& submeshMaterials = m_meshSubmeshMaterials[meshIndex];

				MeshRenderer* renderer = enableRaytracing
					? object->AddComponent<RaytracingMeshRenderer>()
					: object->AddComponent<MeshRenderer>();
				renderer->SetMesh(static_cast<Mesh*>(m_meshes[meshIndex].get()));
				renderer->SetShader(shader);
				const int materialIndex = submeshMaterials.empty() ? -1 : submeshMaterials[0];
				renderer->SetMaterial(MaterialForSubmesh(materialIndex), 0);
			}
		}

		for (int childNode : node.Children)
		{
			object->AddChild(InstantiateNode(childNode, shader, enableRaytracing));
		}

		return object;
	}
}
