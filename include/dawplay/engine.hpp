#pragma once

#include "dawplay/project.hpp"
#include "dawplay/sources.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace dawplay {

enum class PcmFormat { F32, S32, S24, S16, U8 };

struct RenderRequest {
    std::string path;
    int sampleRate = 48000;
    PcmFormat format = PcmFormat::F32;
    bool multichannel = false;
};

struct EngineSettings {
    int outputRate = 48000;
    int outputChannels = 2;
    StretchPreset preset = StretchPreset::Cheaper;
};

class Engine {
public:
    Engine(std::shared_ptr<AudioStore> store, Project project, EngineSettings settings);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    const Project& project() const { return project_; }
    int outputRate() const { return outputRate_; }
    StretchPreset preset() const { return static_cast<StretchPreset>(presetCode_.load()); }

    void play();
    void pause();
    void stop();
    void seekSeconds(double seconds);
    void seekBar(double bar);
    bool playing() const { return playing_.load(); }
    double positionSeconds() const;
    double positionBeats() const;
    double positionBar() const;

    void setStretchPreset(StretchPreset preset);
    void setHardwareOutput(int channelIndex, int startChannel, int width);
    bool renderToWav(const RenderRequest& request, std::string& error);

    bool startDevice(std::string& error);
    void stopDevice();
    void pump(float* output, int frameCount);
    void noteCallback(std::uint64_t nanoseconds, std::uint32_t frames);
    std::vector<std::string> playbackDevices() const;
    int selectedDevice() const { return deviceIndex_; }
    void selectDevice(int index);
    void setCacheBudget(std::size_t bytes);

    MemoryStats memory() const;
    double cpuPercent() const;
    double callbackLoad() const;
    std::uint64_t underruns() const { return underruns_.load(); }

private:
    struct Ring;
    struct Voice;

    void fillLoop();
    void fillAhead();
    void mixBlock(float* deviceOut, int frameCount, bool advance);
    void requestSeek(std::int64_t frame);
    void ensureVoices();
    void prepareMixBuffers();
    int channelCountFor(const AudioEvent& event) const;
    bool eventNeedsStretch(const AudioEvent& event, double ratio, double transpose) const;

    std::shared_ptr<AudioStore> store_;
    Project project_;
    EngineSettings settings_;
    int outputRate_ = 48000;
    int deviceChannels_ = 2;
    std::atomic<int> presetCode_;
    std::vector<int> mixOrder_;
    std::vector<std::atomic<int>> hardwareStart_;
    std::vector<std::atomic<int>> hardwareWidth_;

    std::atomic<bool> playing_{false};
    std::atomic<std::int64_t> playhead_{0};
    std::atomic<std::uint64_t> seekId_{0};
    std::atomic<std::uint64_t> appliedSeek_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> callbackNs_{0};
    std::atomic<std::uint64_t> callbackFrames_{1};
    std::atomic<int> presetEpoch_{0};

    std::vector<std::unique_ptr<Voice>> voices_;
    std::vector<float> scratch_;
    std::vector<float> buses_;
    std::vector<float> post_;
    std::vector<float> popped_;
    std::vector<float> discard_;
    int scratchFrames_ = 256;
    // Voices are published only after the vector slot is fully constructed.
    // The audio thread never grows this vector.
    std::atomic<int> publishedVoices_{0};

    std::thread fillThread_;
    std::atomic<bool> stopThread_{false};
    bool inlineFill_ = false;

    void* context_ = nullptr;
    void* device_ = nullptr;
    bool deviceOpen_ = false;
    int deviceIndex_ = -1;
    std::vector<std::string> deviceNames_;

    mutable std::uint64_t cpuLastWallNs_ = 0;
    mutable std::uint64_t cpuLastProcessNs_ = 0;
    mutable double cpuPercent_ = 0;
};

const char* pcmFormatName(PcmFormat format);
bool pcmFormatFromName(const std::string& name, PcmFormat& format);

}  // namespace dawplay
