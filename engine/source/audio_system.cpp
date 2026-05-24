#include "pch.h"
#include "audio_system.h"
#include "debug_console.h"

namespace udsdx
{
	AudioSystem::AudioSystem()
	{
		std::wstring logMessage = L"Audio Renderer Details:\n\n";
		for (const auto& detail : AudioEngine::GetRendererDetails())
		{
			logMessage += std::format(L"  > Audio Renderer: {} {}\n", detail.description, detail.deviceId);
		}
		DebugConsole::Log(logMessage);

		AUDIO_ENGINE_FLAGS eflags = AudioEngine_Default;
#ifdef _DEBUG
		eflags |= AudioEngine_Debug;
#endif
		m_audioEngine = std::make_unique<DirectX::AudioEngine>(eflags);
		m_audioEngine->SetReverb(Reverb_Cave);
	}

	AudioSystem::~AudioSystem()
	{

	}

	AudioEngine* AudioSystem::GetAudioEngine() const
	{
		return m_audioEngine.get();
	}

	const AudioListener& AudioSystem::GetAudioListener() const
	{
		return m_audioListener;
	}

	void AudioSystem::UpdateAudioListener(const Vector3& position, const Quaternion& orientation)
	{
		if (!m_audioEngine)
		{
			return;
		}

		m_audioListener.SetPosition(position);
		m_audioListener.SetOrientationFromQuaternion(orientation);
	}

	void AudioSystem::Update()
	{
		ZoneScoped;
		if (!m_audioEngine)
		{
			return;
		}

		m_audioEngine->Update();
	}
}
