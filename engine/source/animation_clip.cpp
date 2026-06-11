#include "pch.h"
#include "animation_clip.h"
#include "debug_console.h"
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

	AnimationClip::AnimationClip(const aiScene* scene, const aiAnimation* animationSrc)
	{
		if (scene == nullptr || scene->mRootNode == nullptr || animationSrc == nullptr)
		{
			DebugConsole::LogError("Failed to build animation clip from a null scene or animation.");
			return;
		}

		BuildSkeleton(scene);

		// Precompute each bone's bind-pose TRS so channel-less bones can still be sampled.
		m_bindPoses.resize(m_bones.size());
		for (size_t i = 0; i < m_bones.size(); ++i)
		{
			Matrix4x4 bindTransform = m_bones[i].Transform;
			Vector3 scale;
			Quaternion rotation;
			Vector3 translation;
			if (bindTransform.Decompose(scale, rotation, translation))
			{
				m_bindPoses[i].Position = translation;
				m_bindPoses[i].Rotation = rotation;
				m_bindPoses[i].Scale = scale;
			}
		}

		m_name = animationSrc->mName.C_Str();
		m_ticksPerSecond = static_cast<float>(animationSrc->mTicksPerSecond != 0.0 ? animationSrc->mTicksPerSecond : 1.0);
		m_duration = static_cast<float>(animationSrc->mDuration);
		m_channels.resize(m_bones.size());

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

			auto channelIter = m_boneIndexMap.find(channel.Name);
			if (channelIter == m_boneIndexMap.end())
			{
				continue;
			}
			m_channels[channelIter->second] = std::move(channel);
		}
	}

	void AnimationClip::BuildSkeleton(const aiScene* scene)
	{
		m_bones.clear();
		m_boneParents.clear();
		m_boneIndexMap.clear();

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

	UINT AnimationClip::GetBoneCount() const
	{
		return static_cast<UINT>(m_bones.size());
	}

	bool AnimationClip::SampleLocalPose(int boneIndex, float animationTime, BoneLocalPose& out) const
	{
		if (boneIndex < 0 || boneIndex >= static_cast<int>(m_channels.size()))
		{
			out = BoneLocalPose{};
			return false;
		}

		const Channel& channel = m_channels[boneIndex];
		if (channel.Name.empty())
		{
			out = m_bindPoses[boneIndex];
			return false;
		}

		float animationTicks = animationTime * m_ticksPerSecond;

		auto [ps1, ps2, pf] = ToTimeFraction(channel.PositionTimestamps, animationTicks);
		auto [rs1, rs2, rf] = ToTimeFraction(channel.RotationTimestamps, animationTicks);
		auto [ss1, ss2, sf] = ToTimeFraction(channel.ScaleTimestamps, animationTicks);

		XMVECTOR p0 = XMLoadFloat3(&channel.Positions[ps1]);
		XMVECTOR p1 = XMLoadFloat3(&channel.Positions[ps2]);
		XMStoreFloat3(&out.Position, XMVectorLerp(p0, p1, pf));

		XMVECTOR q0 = XMLoadFloat4(&channel.Rotations[rs1]);
		XMVECTOR q1 = XMLoadFloat4(&channel.Rotations[rs2]);
		XMStoreFloat4(&out.Rotation, XMQuaternionSlerp(q0, q1, rf));

		XMVECTOR s0 = XMLoadFloat3(&channel.Scales[ss1]);
		XMVECTOR s1 = XMLoadFloat3(&channel.Scales[ss2]);
		XMStoreFloat3(&out.Scale, XMVectorLerp(s0, s1, sf));

		return true;
	}
}
