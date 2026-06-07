#include "pch.h"
#include "animation_clip.h"
#include "rigged_mesh.h"
#include "debug_console.h"
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>


namespace udsdx
{
	static std::tuple<size_t, size_t, float> ToTimeFraction(const std::vector<float>& timeStamps, float time)
	{
		auto size = timeStamps.size();
		auto seg = std::distance(timeStamps.begin(), std::lower_bound(timeStamps.begin(), timeStamps.end(), time));
		if (seg == 0)
		{
			return { 0, size - 1, 0.0f };
		}
		if (seg == size)
		{
			return { 0, size - 1, 1.0f };
		}
		float begin = timeStamps[seg - 1];
		float end = timeStamps[seg];
		float fraction = (time - begin) / (end - begin);
		return { seg - 1, seg, fraction };
	}

	AnimationClip::AnimationClip(const std::filesystem::path& resourcePath)
	{
		Assimp::Importer importer;
		importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
		const aiScene* scene = importer.ReadFile(
			resourcePath.string(),
			aiProcess_ConvertToLeftHanded |
			aiProcess_Triangulate |
			aiProcess_GenNormals |
			aiProcess_CalcTangentSpace |
			aiProcess_LimitBoneWeights |
			aiProcess_OptimizeMeshes |
			aiProcess_RemoveRedundantMaterials
		);
		if (scene == nullptr || scene->mRootNode == nullptr)
		{
			DebugConsole::LogError("Failed to load animation clip with assimp: " + resourcePath.string());
			return;
		}

		Build(scene);
	}

	AnimationClip::AnimationClip(const aiScene* scene)
	{
		if (scene == nullptr || scene->mRootNode == nullptr)
		{
			DebugConsole::LogError("Failed to build animation clip from a null scene.");
			return;
		}

		Build(scene);
	}

	void AnimationClip::Build(const aiScene* scene)
	{
		m_bones.clear();
		m_boneParents.clear();
		m_boneIndexMap.clear();
		m_animations.clear();

		std::vector<std::pair<aiNode*, int>> nodeStack;
		nodeStack.emplace_back(scene->mRootNode, -1);
		while (!nodeStack.empty())
		{
			auto [node, parentIndex] = nodeStack.back();
			nodeStack.pop_back();

			Bone boneData{};
			boneData.Name = node->mName.C_Str();
			aiMatrix4x4 transposed = node->mTransformation.Transpose();
			boneData.Transform = XMFLOAT4X4(reinterpret_cast<float*>(&transposed.a1));

			m_boneIndexMap[boneData.Name] = static_cast<int>(m_bones.size());
			m_bones.emplace_back(std::move(boneData));
			m_boneParents.emplace_back(parentIndex);

			for (unsigned int childIndex = 0; childIndex < node->mNumChildren; ++childIndex)
			{
				nodeStack.emplace_back(node->mChildren[childIndex], static_cast<int>(m_bones.size()) - 1);
			}
		}

		for (unsigned int i = 0; i < scene->mNumAnimations; ++i)
		{
			Animation animationDest(this, scene->mAnimations[i], m_boneIndexMap, static_cast<unsigned int>(m_bones.size()));
			m_animations.emplace(animationDest.GetName().data(), std::move(animationDest));
		}
	}

	void AnimationClip::PopulateBoneMap(const std::vector<std::string>& boneNames, std::vector<int>& out) const
	{
		out.resize(boneNames.size());
		for (UINT i = 0; i < out.size(); ++i)
		{
			out[i] = GetBoneIndex(boneNames[i]);
		}
	}

	int AnimationClip::GetBoneIndex(std::string_view boneName) const
	{
		auto it = m_boneIndexMap.find(boneName.data());
		if (it == m_boneIndexMap.end())
		{
			return -1;
		}
		return it->second;
	}

	const Animation& AnimationClip::GetAnimation(std::string_view name) const
	{
		auto it = m_animations.find(name.data());
		if (it == m_animations.end())
		{
			DebugConsole::LogError("Animation not found: " + std::string(name));
			throw std::runtime_error("Animation not found");
		}
		return it->second;
	}

	const Animation& AnimationClip::GetAnimation() const
	{
		if (m_animations.empty())
		{
			DebugConsole::LogError("No animations available in the clip.");
			throw std::runtime_error("No animations available");
		}
		return m_animations.begin()->second;
	}

	UINT AnimationClip::GetBoneCount() const
	{
		return static_cast<UINT>(m_bones.size());
	}

	Animation::Animation(const AnimationClip* clip, const aiAnimation* animationSrc, const std::unordered_map<std::string, int>& boneIndexMap, unsigned int boneCount) : m_clip(clip)
	{
		m_name = animationSrc->mName.C_Str();
		m_ticksPerSecond = static_cast<float>(animationSrc->mTicksPerSecond != 0.0 ? animationSrc->mTicksPerSecond : 1.0);
		m_duration = static_cast<float>(animationSrc->mDuration);
		m_channels.resize(boneCount);

		for (unsigned int i = 0; i < animationSrc->mNumChannels; ++i)
		{
			const aiNodeAnim* channelSrc = animationSrc->mChannels[i];
			Channel channel{};
			channel.Name = channelSrc->mNodeName.C_Str();
			for (unsigned int j = 0; j < channelSrc->mNumPositionKeys; ++j)
			{
				const aiVectorKey& key = channelSrc->mPositionKeys[j];
				channel.PositionTimestamps.push_back(static_cast<float>(key.mTime));
				channel.Positions.emplace_back(key.mValue.x, key.mValue.y, key.mValue.z);
			}
			for (unsigned int j = 0; j < channelSrc->mNumRotationKeys; ++j)
			{
				const aiQuatKey& key = channelSrc->mRotationKeys[j];
				channel.RotationTimestamps.push_back(static_cast<float>(key.mTime));
				channel.Rotations.emplace_back(key.mValue.x, key.mValue.y, key.mValue.z, key.mValue.w);
			}
			for (unsigned int j = 0; j < channelSrc->mNumScalingKeys; ++j)
			{
				const aiVectorKey& key = channelSrc->mScalingKeys[j];
				channel.ScaleTimestamps.push_back(static_cast<float>(key.mTime));
				channel.Scales.emplace_back(key.mValue.x, key.mValue.y, key.mValue.z);
			}

			auto channelIter = boneIndexMap.find(channel.Name);
			if (channelIter == boneIndexMap.end())
			{
				continue;
			}
			m_channels[channelIter->second] = std::move(channel);
		}
	}

	void Animation::PopulateTransforms(float animationTime, std::vector<Matrix4x4>& out) const
	{
		std::vector<int> boneMap;

		boneMap.reserve(m_clip->GetBoneCount());

		int index = 0;
		for (const Bone& bone : m_clip->GetBones())
		{
			boneMap.push_back(index++);
		}

		PopulateTransforms(animationTime, boneMap, out);
	}

	void Animation::PopulateTransforms(float animationTime, const std::vector<int>& boneMap, std::vector<Matrix4x4>& out, const std::map<std::string_view, Matrix4x4>& modifiers) const
	{
		UINT boneCount = static_cast<UINT>(m_clip->GetBoneCount());

		float animationTicks = animationTime * m_ticksPerSecond;
		std::vector<Matrix4x4> in(boneCount);

		for (UINT i = 0; i < boneCount; ++i)
		{
			const Bone& bone = m_clip->GetBones().at(i);
			const Animation::Channel& channel = m_channels[i];

			XMMATRIX tParent = XMMatrixIdentity();
			if (m_clip->GetBoneParents().at(i) != -1)
			{
				tParent = XMLoadFloat4x4(&in[m_clip->GetBoneParents().at(i)]);
			}

			XMMATRIX tLocal;
			if (channel.Name.empty())
				tLocal = XMLoadFloat4x4(&bone.Transform);
			else
			{
				auto [ps1, ps2, pf] = ToTimeFraction(channel.PositionTimestamps, animationTicks);
				auto [rs1, rs2, rf] = ToTimeFraction(channel.RotationTimestamps, animationTicks);
				auto [ss1, ss2, sf] = ToTimeFraction(channel.ScaleTimestamps, animationTicks);

				XMVECTOR p0 = XMLoadFloat3(&channel.Positions[ps1]);
				XMVECTOR p1 = XMLoadFloat3(&channel.Positions[ps2]);
				XMVECTOR p = XMVectorLerp(p0, p1, pf);

				XMVECTOR q0 = XMLoadFloat4(&channel.Rotations[rs1]);
				XMVECTOR q1 = XMLoadFloat4(&channel.Rotations[rs2]);
				XMVECTOR q = XMQuaternionSlerp(q0, q1, rf);

				XMVECTOR s0 = XMLoadFloat3(&channel.Scales[ss1]);
				XMVECTOR s1 = XMLoadFloat3(&channel.Scales[ss2]);
				XMVECTOR s = XMVectorLerp(s0, s1, sf);

				tLocal = XMMatrixAffineTransformation(s, XMVectorZero(), q, p);
				if (modifiers.find(bone.Name) != modifiers.end())
				{
					XMMATRIX modifier = XMLoadFloat4x4(&modifiers.at(bone.Name));
					tLocal = tLocal * modifier;
				}
			}

			XMStoreFloat4x4(&in[i], tLocal * tParent);
		}

		out.resize(boneMap.size());
		for (UINT i = 0; i < out.size(); ++i)
		{
			int boneID = boneMap[i];
			XMMATRIX boneTransform = boneID >= 0 ? XMLoadFloat4x4(&in[boneID]) : XMMatrixIdentity();
			XMStoreFloat4x4(&out[i], boneTransform);
		}
	}
}