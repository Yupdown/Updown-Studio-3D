#include "pch.h"
#include "animator.h"
#include "animation_clip.h"
#include "scene_object.h"
#include "transform.h"
#include "debug_console.h"

namespace udsdx
{
	void Animator::Update(const Time& time, Scene& scene)
	{
		if (m_clip == nullptr)
		{
			return;
		}

		m_time += time.deltaTime;
		m_prevTime += time.deltaTime;
		m_transitionFactor += time.deltaTime / 0.2f;

		float sampleTime = m_loop ? fmodf(m_time, m_clip->GetAnimationDuration()) : m_time;
		const auto& bindings = GetClipBindings(m_clip);
		const auto& bones = m_clip->GetBones();

		bool blending = m_prevClip != nullptr;
		float t = blending ? SmoothStep(std::clamp(m_transitionFactor, 0.0f, 1.0f)) : 1.0f;
		float prevSampleTime = (blending && m_prevLoop) ? fmodf(m_prevTime, m_prevClip->GetAnimationDuration()) : m_prevTime;

		for (size_t i = 0; i < bindings.size(); ++i)
		{
			Transform* target = bindings[i];
			if (target == nullptr)
			{
				continue;
			}

			BoneLocalPose pose;
			bool animated = m_clip->SampleLocalPose(static_cast<int>(i), sampleTime, pose);

			if (blending)
			{
				int prevIndex = m_prevClip->GetBoneIndex(bones[i].Name);
				BoneLocalPose prevPose;
				bool prevAnimated = prevIndex >= 0 && m_prevClip->SampleLocalPose(prevIndex, prevSampleTime, prevPose);

				// Bones untouched by both clips keep whatever pose they have.
				if (!animated && (prevIndex < 0 || !prevAnimated))
				{
					continue;
				}

				pose.Position = Vector3::Lerp(prevPose.Position, pose.Position, t);
				pose.Rotation = Quaternion::Slerp(prevPose.Rotation, pose.Rotation, t);
				pose.Scale = Vector3::Lerp(prevPose.Scale, pose.Scale, t);
			}
			else if (!animated)
			{
				continue;
			}

			target->SetLocalPosition(pose.Position);
			target->SetLocalRotation(pose.Rotation);
			target->SetLocalScale(pose.Scale);
		}

		// The crossing frame above still blended at t == 1, so the previous clip can be dropped.
		if (blending && m_transitionFactor >= 1.0f)
		{
			m_prevClip = nullptr;
		}
	}

	void Animator::AddClip(const AnimationClip* clip)
	{
		if (clip == nullptr)
		{
			return;
		}
		m_clips.push_back(clip);
		m_clipMap.emplace(std::string(clip->GetName()), clip);
	}

	const AnimationClip* Animator::GetClip(std::string_view name) const
	{
		auto iter = m_clipMap.find(std::string(name));
		return iter != m_clipMap.end() ? iter->second : nullptr;
	}

	void Animator::Play(std::string_view name, bool loop, bool forcePlay)
	{
		const AnimationClip* clip = GetClip(name);
		if (clip == nullptr)
		{
			DebugConsole::LogWarning("Animator: no clip registered under the given name.");
			return;
		}
		Play(clip, loop, forcePlay);
	}

	void Animator::Play(const AnimationClip* clip, bool loop, bool forcePlay)
	{
		if (clip == nullptr || (!forcePlay && m_clip == clip))
		{
			return;
		}

		// If the animation is not blending
		if (m_transitionFactor >= 1.0f || forcePlay)
		{
			m_prevClip = m_clip;
			m_prevLoop = m_loop;
			// Capture the looped time so the previous pose does not snap to the clip's end.
			m_prevTime = (m_prevClip != nullptr && m_loop) ? fmodf(m_time, m_prevClip->GetAnimationDuration()) : m_time;
			m_time = 0.0f;
			m_transitionFactor = 0.0f;
		}
		// If the animation is blending, but the new animation is the previous one
		else if (clip == m_prevClip)
		{
			m_prevClip = m_clip;
			m_transitionFactor = 1.0f - m_transitionFactor;
			std::swap(m_time, m_prevTime);
			std::swap(m_loop, m_prevLoop);
		}
		// If the animation is blending, but the new animation is different from the previous one
		else
		{
			m_time = 0.0f;
		}

		m_clip = clip;
		m_loop = loop;
	}

	void Animator::Stop()
	{
		m_clip = nullptr;
		m_prevClip = nullptr;
		m_time = 0.0f;
		m_prevTime = 0.0f;
		m_transitionFactor = 1.0f;
	}

	bool Animator::IsPlaying() const
	{
		return m_clip != nullptr && (m_loop || m_time < m_clip->GetAnimationDuration());
	}

	void Animator::SetTransitionFactor(float factor)
	{
		m_transitionFactor = factor;
	}

	void Animator::RebindBones()
	{
		m_transformMapValid = false;
		m_transformMap.clear();
		m_clipBindings.clear();
	}

	void Animator::EnsureTransformMap()
	{
		if (m_transformMapValid)
		{
			return;
		}
		m_transformMap.clear();
		SceneObject::Enumerate(GetSceneObject(), [this](const std::shared_ptr<SceneObject>& node) {
			if (!node->GetName().empty())
			{
				m_transformMap.emplace(node->GetName(), node->GetTransform());
			}
		}, false);
		m_transformMapValid = true;
	}

	const std::vector<Transform*>& Animator::GetClipBindings(const AnimationClip* clip)
	{
		auto iter = m_clipBindings.find(clip);
		if (iter != m_clipBindings.end())
		{
			return iter->second;
		}

		EnsureTransformMap();

		std::vector<Transform*> bindings(clip->GetBoneCount(), nullptr);
		const auto& bones = clip->GetBones();
		for (size_t i = 0; i < bones.size(); ++i)
		{
			auto found = m_transformMap.find(bones[i].Name);
			if (found == m_transformMap.end())
			{
				continue;
			}
			// The skeleton root resolves to the Animator's own object; leave it nullptr so the
			// user-owned root transform is never overwritten.
			if (found->second == GetSceneObject()->GetTransform())
			{
				continue;
			}
			bindings[i] = found->second;
		}
		return m_clipBindings.emplace(clip, std::move(bindings)).first->second;
	}
}
