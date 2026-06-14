#pragma once

#include "pch.h"
#include "component.h"

namespace udsdx
{
	class AnimationClip;
	class Transform;

	// Plays AnimationClips on a skeleton made of real SceneObjects: every Update samples the
	// current clip (blending 0.2s with the previous one on switch) and writes the resulting local
	// TRS into the bone SceneObjects' Transforms, resolved by name among the descendants. The
	// skeleton root (the Animator's own object) is never written: the user owns that transform.
	// A RiggedMeshRenderer on the same object then reads those Transforms for skinning.
	class Animator : public Component
	{
	public:
		virtual void Update(const Time& time, Scene& scene) override;

		// Registers a clip, keyed by clip->GetName(). Clips are owned by the ModelAsset.
		void AddClip(const AnimationClip* clip);
		const AnimationClip* GetClip(std::string_view name) const;

		void Play(std::string_view name, bool loop = false, bool forcePlay = false);
		void Play(const AnimationClip* clip, bool loop = false, bool forcePlay = false);
		// Stops playback and clears the current/blended clips. Update then leaves the bone
		// Transforms untouched, so they hold whatever pose they currently have (e.g. the
		// instantiated bind pose if Stop is called before the first Update).
		void Stop();
		bool IsPlaying() const;
		const AnimationClip* GetCurrentClip() const { return m_clip; }
		void SetTransitionFactor(float factor);

		// Re-resolves bone-name -> Transform bindings from the current descendants.
		// Call after re-parenting or renaming objects under the Animator.
		void RebindBones();

	private:
		const std::vector<Transform*>& GetClipBindings(const AnimationClip* clip);
		void EnsureTransformMap();

		std::vector<const AnimationClip*> m_clips;
		std::unordered_map<std::string, const AnimationClip*> m_clipMap;

		const AnimationClip* m_clip = nullptr;
		const AnimationClip* m_prevClip = nullptr;
		float m_time = 0.0f;
		float m_prevTime = 0.0f;
		bool m_loop = false;
		bool m_prevLoop = false;
		float m_transitionFactor = 1.0f;

		bool m_transformMapValid = false;
		std::unordered_map<std::string, Transform*> m_transformMap;
		std::unordered_map<const AnimationClip*, std::vector<Transform*>> m_clipBindings;
	};
}
