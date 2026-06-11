#pragma once

#include "pch.h"
#include "resource_object.h"

struct aiAnimation;
struct aiScene;

namespace udsdx
{
	// One node of the clip's own skeleton: the name resolves the bone to a SceneObject, the
	// transform is the bind-pose local matrix used when a bone has no animation channel.
	struct Bone
	{
		std::string Name{};
		Matrix4x4 Transform{};
	};

	// Local TRS of one bone, as sampled from an animation channel or taken from the bind pose.
	struct BoneLocalPose
	{
		Vector3 Position = Vector3::Zero;
		Quaternion Rotation = Quaternion::Identity;
		Vector3 Scale = Vector3::One;
	};

	// A single animation together with the skeleton it plays on. ModelAsset builds one clip per
	// source animation, looked up by key. Sampling reads the clip's own bones, so the clip is
	// fully self-contained.
	class AnimationClip : public ResourceObject
	{
	private:
		struct Channel
		{
			std::string Name{};

			std::vector<float> PositionTimestamps{};
			std::vector<float> RotationTimestamps{};
			std::vector<float> ScaleTimestamps{};

			std::vector<Vector3> Positions{};
			std::vector<Quaternion> Rotations{};
			std::vector<Vector3> Scales{};
		};

	public:
		AnimationClip(const aiScene* scene, const aiAnimation* animationSrc); 

	public:
		int GetBoneIndex(std::string_view boneName) const;
		const std::vector<Bone>& GetBones() const { return m_bones; }
		const std::vector<int>& GetBoneParents() const { return m_boneParents; }
		UINT GetBoneCount() const;

		// Samples the channel of clip-bone 'boneIndex' at 'animationTime' (seconds). Returns true
		// if the bone has an animation channel; otherwise writes the bind pose and returns false.
		bool SampleLocalPose(int boneIndex, float animationTime, BoneLocalPose& out) const;
		const BoneLocalPose& GetBindPose(int boneIndex) const { return m_bindPoses[boneIndex]; }

		float GetAnimationDuration() const { return m_duration / m_ticksPerSecond; }
		std::string_view GetName() const { return m_name; }

	protected:
		// Walks the scene node tree into m_bones / m_boneParents / m_boneIndexMap.
		void BuildSkeleton(const aiScene* scene);

		std::vector<Bone> m_bones;
		std::vector<int> m_boneParents;
		std::unordered_map<std::string, int> m_boneIndexMap;
		std::vector<BoneLocalPose> m_bindPoses;

		std::vector<Channel> m_channels;
		std::string m_name;
		float m_duration = 0.0f;
		float m_ticksPerSecond = 30.0f;
	};
}