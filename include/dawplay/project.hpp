#pragma once

#include "dawplay/timeline.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dawplay {

struct FileRef {
    std::string path;
    bool external = false;
    int channels = 2;
    int sampleRate = 48000;
    double duration = 0;
    std::string algorithm;
    // Index into AudioStore's path table. Set on first read; -1 until then.
    mutable int storeId = -1;
};

// One placement step from an outer timeline into the clip's content clock.
struct ClipLayer {
    TimeUnit parentUnit = TimeUnit::Beats;
    TimeUnit contentUnit = TimeUnit::Beats;
    double origin = 0;
    double duration = 0;
    double playStart = 0;
    double playStop = 0;
    double loopStart = 0;
    double loopEnd = 0;
    double scheduleOrigin = 0;
};

struct LayerAutomation {
    int layer = 0;
    AutomationCurve gain;
    AutomationCurve pan;
    AutomationCurve transpose;
};

struct AudioEvent {
    FileRef file;
    std::vector<ClipLayer> layers;
    bool hasWarp = false;
    WarpMap warp;
    std::vector<LayerAutomation> automation;
    double startSecond = 0;
    double endSecond = 0;
    double fadeInSecond = 0;
    double fadeOutSecond = 0;
    bool equalPowerIn = false;
    bool equalPowerOut = false;
    int channelIndex = 0;
    std::string name;

    bool sourceAt(const TempoMap& tempo, double arrangementSecond, double& sourceSecond) const;
    double contentTime(const TempoMap& tempo, double arrangementSecond, int layerIndex) const;
};

struct MixerChannel {
    std::string id;
    std::string name;
    std::string role;
    std::string destinationId;
    int destination = -1;
    int audioChannels = 2;
    bool solo = false;
    bool audible = true;
    AutomationCurve volume;
    AutomationCurve pan;
    AutomationCurve mute;
    int hardwareStart = -1;
    int hardwareWidth = 2;
};

struct Project {
    std::string application;
    TempoMap tempo;
    TimeSigMap timeSignature;
    std::vector<MixerChannel> channels;
    std::vector<AudioEvent> events;
    std::vector<std::string> warnings;
    int masterIndex = -1;
    size_t modelBytes = 0;

    double lengthSeconds() const;
    bool anySolo() const;
};

struct AudioStore;

struct LoadResult {
    Project project;
    std::shared_ptr<AudioStore> store;
    std::string error;
};

LoadResult loadProjectFile(const std::string& path);
LoadResult loadProjectBytes(const std::uint8_t* data, std::size_t size, const std::string& label);

}  // namespace dawplay
