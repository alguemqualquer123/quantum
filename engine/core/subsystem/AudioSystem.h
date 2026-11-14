// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef AUDIOSYSTEM_H
#define AUDIOSYSTEM_H

#include "SubSystem.h"

#include "component/SoundComponent.h"

#include <memory>
#include <string>

namespace SoLoud{
	class Soloud;
	class WavStream;
}

namespace quantum{

	class QUANTUM_API AudioSystem : public SubSystem {

    private:
		static bool inited;

		static void deInit();

		static float globalVolume;
		static bool muted;
		static std::shared_ptr<SoLoud::WavStream> musicSource;
		static std::shared_ptr<SoLoud::WavStream> nextMusicSource;
		static unsigned int musicHandle;
		static unsigned int nextMusicHandle;
		static float musicFadeDuration;
		static float musicFadeElapsed;

		Vector3 cameraLastPosition;
		bool outputRequested = false;

        bool loadSoundSample(SoundComponent& audio);
        void preloadSoundAssets();

	public:
		AudioSystem(Scene* scene);

		static SoLoud::Soloud& getSoloud();
		static bool init();

		static void stopAll();
		static void pauseAll();
		static void resumeAll();
		static void checkActive();

		static void setGlobalVolume(float volume);
		static float getGlobalVolume();
		// Converts a position delta to the velocity expected by SoLoud's 3D
		// source/listener API. A non-positive delta deliberately yields zero.
		static Vector3 calculateDopplerVelocity(const Vector3& current,
			const Vector3& previous, double deltaSeconds);
		static void setBusVolume(const std::string& bus, float volume);
		static float getBusVolume(const std::string& bus);

		// Platform mute, like YouTube's, that the global volume can't override
		static void setMuted(bool muted);
		static bool loadMusicStream(const std::string& filename);
		static bool playMusicStream(const std::string& filename, float crossFadeSeconds = 1.0f);
		static void stopMusic();
		static bool isMusicPlaying();
		static bool setAmbienceLayer(const std::string& name, const std::string& filename,
			float volume = 1.0f, float fadeSeconds = 1.0f);
		static void stopAmbienceLayer(const std::string& name, float fadeSeconds = 1.0f);
		static void stopAmbience();
		static size_t getAmbienceLayerCount();

        bool loadSound(SoundComponent& audio);
		void destroySound(SoundComponent& audio);
		void getLoadCount(size_t& loaded, size_t& total) const;
		void stopSceneSounds();
		bool seekSound(SoundComponent& audio, double time);

		void setPaused(bool paused) override;

		void load() override;
		void draw() override;
		void destroy() override;
		void update(double dt) override;

		void onComponentAdded(Entity entity, ComponentId componentId) override;
		void onComponentRemoved(Entity entity, ComponentId componentId) override;
	};

}

#endif //AUDIOSYSTEM_H
