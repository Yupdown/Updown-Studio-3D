#pragma once

#include "pch.h"
#include "resource_object.h"

struct aiAnimation;
struct aiScene;

namespace udsdx
{
	struct Bone;

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
		void PopulateBoneMap(const std::vector<std::string>& boneNames, std::vector<int>& out) const;
		int GetBoneIndex(std::string_view boneName) const;
		const std::vector<Bone>& GetBones() const { return m_bones; }
		const std::vector<int>& GetBoneParents() const { return m_boneParents; }
		UINT GetBoneCount() const;

		// Samples the animation at 'animationTime' (seconds) into local bone transforms.
		void PopulateTransforms(float animationTime, std::vector<Matrix4x4>& out) const;
		void PopulateTransforms(float animationTime, const std::vector<int>& boneMap, std::vector<Matrix4x4>& out, const std::map<std::string_view, Matrix4x4>& modifiers = {}) const;

		float GetAnimationDuration() const { return m_duration / m_ticksPerSecond; }
		std::string_view GetName() const { return m_name; }

	protected:
		// Walks the scene node tree into m_bones / m_boneParents / m_boneIndexMap.
		void BuildSkeleton(const aiScene* scene);

		std::vector<Bone> m_bones;
		std::vector<int> m_boneParents;
		std::unordered_map<std::string, int> m_boneIndexMap;

		std::vector<Channel> m_channels;
		std::string m_name;
		float m_duration = 0.0f;
		float m_ticksPerSecond = 30.0f;
	};
}