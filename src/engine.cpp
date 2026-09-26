#include "dawplay/engine.hpp"

#if defined(__EMSCRIPTEN__)
#define MA_ENABLE_AUDIO_WORKLETS
#endif
#include "miniaudio.h"
#include "signalsmith-stretch/signalsmith-stretch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <sys/resource.h>

#if defined(__linux__)
#include <unistd.h>
#endif
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

namespace dawplay {
namespace {

constexpr int kMaxStretchers = 16;
constexpr int kProduceFrames = 256;

double timeSeconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

std::uint64_t processCpuNs() {
#if defined(__linux__) || defined(__APPLE__)
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
    const std::uint64_t user = static_cast<std::uint64_t>(usage.ru_utime.tv_sec) * 1000000000ull +
                               static_cast<std::uint64_t>(usage.ru_utime.tv_usec) * 1000ull;
    const std::uint64_t system = static_cast<std::uint64_t>(usage.ru_stime.tv_sec) * 1000000000ull +
                                 static_cast<std::uint64_t>(usage.ru_stime.tv_usec) * 1000ull;
    return user + system;
#else
    return 0;
#endif
}

size_t residentBytes() {
#if defined(__linux__)
    FILE* status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) {
        return 0;
    }
    char line[256];
    size_t bytes = 0;
    while (std::fgets(line, sizeof(line), status) != nullptr) {
        unsigned long kb = 0;
        if (std::sscanf(line, "VmRSS: %lu", &kb) == 1) {
            bytes = static_cast<size_t>(kb) * 1024;
            break;
        }
    }
    std::fclose(status);
    return bytes;
#elif defined(__EMSCRIPTEN__)
    return static_cast<size_t>(emscripten_get_heap_size());
#else
    return 0;
#endif
}

struct Planar {
    float* data = nullptr;
    int frames = 0;
    float* operator[](int channel) const { return data + static_cast<size_t>(channel) * static_cast<size_t>(frames); }
};

void interleaveToPlanar(const float* interleaved, float* planar, int frames, int channels) {
    for (int channel = 0; channel < channels; ++channel) {
        for (int frame = 0; frame < frames; ++frame) {
            planar[static_cast<size_t>(channel) * static_cast<size_t>(frames) + static_cast<size_t>(frame)] =
                interleaved[static_cast<size_t>(frame) * static_cast<size_t>(channels) + static_cast<size_t>(channel)];
        }
    }
}

void planarToInterleave(const float* planar, float* interleaved, int frames, int channels) {
    for (int channel = 0; channel < channels; ++channel) {
        for (int frame = 0; frame < frames; ++frame) {
            interleaved[static_cast<size_t>(frame) * static_cast<size_t>(channels) + static_cast<size_t>(channel)] =
                planar[static_cast<size_t>(channel) * static_cast<size_t>(frames) + static_cast<size_t>(frame)];
        }
    }
}

size_t estimateStretchBytes(const signalsmith::stretch::SignalsmithStretch<float>& stretch, int channels) {
    const int block = std::max(stretch.blockSamples(), 1);
    const int interval = std::max(stretch.intervalSamples(), 1);
    int fft = 1;
    while (fft < block) {
        fft *= 2;
    }
    const int bins = fft / 2 + 1;
    size_t bytes = sizeof(stretch);
    bytes += static_cast<size_t>(block + interval) * static_cast<size_t>(channels) * sizeof(float);
    bytes += static_cast<size_t>(block) * static_cast<size_t>(channels) * sizeof(float);
    bytes += static_cast<size_t>(bins) * static_cast<size_t>(channels) * sizeof(std::complex<float>) * 6;
    bytes += static_cast<size_t>(fft) * sizeof(float) * 8;
    bytes += static_cast<size_t>(std::max(stretch.outputLatency(), 0)) * static_cast<size_t>(channels) * sizeof(float);
    return bytes;
}

std::vector<int> mixOrder(const Project& project) {
    const int count = static_cast<int>(project.channels.size());
    std::vector<int> incoming(static_cast<size_t>(count), 0);
    for (int channelIndex = 0; channelIndex < count; ++channelIndex) {
        const int destination = project.channels[static_cast<size_t>(channelIndex)].destination;
        if (destination >= 0 && destination < count && destination != channelIndex) {
            ++incoming[static_cast<size_t>(destination)];
        }
    }
    std::vector<int> order;
    std::vector<int> ready;
    for (int channelIndex = 0; channelIndex < count; ++channelIndex) {
        if (incoming[static_cast<size_t>(channelIndex)] == 0) {
            ready.push_back(channelIndex);
        }
    }
    std::vector<std::vector<int>> children(static_cast<size_t>(count));
    for (int channelIndex = 0; channelIndex < count; ++channelIndex) {
        const int destination = project.channels[static_cast<size_t>(channelIndex)].destination;
        if (destination >= 0 && destination < count && destination != channelIndex) {
            children[static_cast<size_t>(channelIndex)].push_back(destination);
        }
    }
    for (size_t readyIndex = 0; readyIndex < ready.size(); ++readyIndex) {
        const int channelIndex = ready[readyIndex];
        order.push_back(channelIndex);
        for (int destination : children[static_cast<size_t>(channelIndex)]) {
            if (--incoming[static_cast<size_t>(destination)] == 0) {
                ready.push_back(destination);
            }
        }
    }
    for (int channelIndex = 0; channelIndex < count; ++channelIndex) {
        if (std::find(order.begin(), order.end(), channelIndex) == order.end()) {
            order.push_back(channelIndex);
        }
    }
    return order;
}

ma_format toMiniFormat(PcmFormat format) {
    switch (format) {
    case PcmFormat::S32:
        return ma_format_s32;
    case PcmFormat::S24:
        return ma_format_s24;
    case PcmFormat::S16:
        return ma_format_s16;
    case PcmFormat::U8:
        return ma_format_u8;
    case PcmFormat::F32:
    default:
        return ma_format_f32;
    }
}

}  // namespace

struct Engine::Ring {
    std::vector<float> samples;
    int channels = 1;
    int capacity = 1;
    std::atomic<std::uint32_t> readCursor{0};
    std::atomic<std::uint32_t> writeCursor{0};

    void configure(int channelCount, int frameCapacity) {
        channels = std::max(channelCount, 1);
        int power = 1;
        while (power < frameCapacity) {
            power *= 2;
        }
        capacity = power;
        samples.assign(static_cast<size_t>(capacity) * static_cast<size_t>(channels), 0.0f);
        readCursor.store(0);
        writeCursor.store(0);
    }

    void clear() {
        readCursor.store(0);
        writeCursor.store(0);
    }

    int filled() const {
        return static_cast<int>(writeCursor.load() - readCursor.load());
    }

    int space() const { return capacity - filled(); }

    void push(const float* interleaved, int frames) {
        const int count = std::min(frames, space());
        std::uint32_t write = writeCursor.load();
        for (int frame = 0; frame < count; ++frame) {
            const size_t slot = static_cast<size_t>((write + static_cast<std::uint32_t>(frame)) &
                                                    static_cast<std::uint32_t>(capacity - 1));
            for (int channel = 0; channel < channels; ++channel) {
                samples[slot * static_cast<size_t>(channels) + static_cast<size_t>(channel)] =
                    interleaved[static_cast<size_t>(frame) * static_cast<size_t>(channels) + static_cast<size_t>(channel)];
            }
        }
        writeCursor.store(write + static_cast<std::uint32_t>(count));
    }

    int pop(float* interleaved, int frames) {
        const int count = std::min(frames, filled());
        std::uint32_t read = readCursor.load();
        for (int frame = 0; frame < count; ++frame) {
            const size_t slot = static_cast<size_t>((read + static_cast<std::uint32_t>(frame)) &
                                                    static_cast<std::uint32_t>(capacity - 1));
            for (int channel = 0; channel < channels; ++channel) {
                interleaved[static_cast<size_t>(frame) * static_cast<size_t>(channels) + static_cast<size_t>(channel)] =
                    samples[slot * static_cast<size_t>(channels) + static_cast<size_t>(channel)];
            }
        }
        readCursor.store(read + static_cast<std::uint32_t>(count));
        return count;
    }
};

struct Engine::Voice {
    int eventIndex = -1;
    std::atomic<bool> stretching{false};
    std::atomic<bool> active{false};
    std::unique_ptr<signalsmith::stretch::SignalsmithStretch<float>> stretch;
    int channels = 1;
    Ring ring;
    bool primed = false;
    double sourceCursor = 0;
    std::atomic<std::int64_t> readFrame{0};
    std::int64_t writeFrame = 0;
    size_t stretchBytes = 0;
    std::vector<float> inputPlanar;
    std::vector<float> outputPlanar;
    std::vector<float> interleaved;
};

Engine::Engine(std::shared_ptr<AudioStore> store, Project project, EngineSettings settings)
    : store_(std::move(store)),
      project_(std::move(project)),
      settings_(settings),
      outputRate_(std::max(settings.outputRate, 8000)),
      deviceChannels_(std::max(settings.outputChannels, 2)),
      presetCode_(static_cast<int>(settings.preset)),
      hardwareStart_(project_.channels.size()),
      hardwareWidth_(project_.channels.size()) {
    mixOrder_ = mixOrder(project_);
    voices_.reserve(128);
    for (size_t channelIndex = 0; channelIndex < project_.channels.size(); ++channelIndex) {
        hardwareStart_[channelIndex].store(project_.channels[channelIndex].hardwareStart);
        hardwareWidth_[channelIndex].store(std::max(project_.channels[channelIndex].hardwareWidth, 1));
    }
    prepareMixBuffers();
    context_ = new ma_context();
    if (ma_context_init(nullptr, 0, nullptr, static_cast<ma_context*>(context_)) != MA_SUCCESS) {
        delete static_cast<ma_context*>(context_);
        context_ = nullptr;
    } else {
        ma_device_info* infos = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(static_cast<ma_context*>(context_), &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
            for (ma_uint32 device = 0; device < count; ++device) {
                deviceNames_.emplace_back(infos[device].name);
            }
        }
    }
    cpuLastWallNs_ = static_cast<std::uint64_t>(timeSeconds() * 1.0e9);
    cpuLastProcessNs_ = processCpuNs();
}

Engine::~Engine() {
    stopThread_.store(true);
    if (fillThread_.joinable()) {
        fillThread_.join();
    }
    stopDevice();
    if (context_ != nullptr) {
        ma_context_uninit(static_cast<ma_context*>(context_));
        delete static_cast<ma_context*>(context_);
    }
}

void Engine::play() {
    if (!fillThread_.joinable()) {
        stopThread_.store(false);
        fillThread_ = std::thread([this] { fillLoop(); });
    }
    playing_.store(true);
}

void Engine::pause() { playing_.store(false); }

void Engine::stop() {
    playing_.store(false);
    requestSeek(0);
}

void Engine::seekSeconds(double seconds) {
    const auto frame = static_cast<std::int64_t>(std::llround(std::max(0.0, seconds) * outputRate_));
    requestSeek(frame);
}

void Engine::seekBar(double bar) {
    const double beats = project_.timeSignature.beatsAtBar(std::max(1.0, bar));
    seekSeconds(project_.tempo.secondsAtBeat(beats));
}

double Engine::positionSeconds() const {
    return static_cast<double>(playhead_.load()) / static_cast<double>(outputRate_);
}

double Engine::positionBeats() const { return project_.tempo.beatsAtSeconds(positionSeconds()); }

double Engine::positionBar() const { return project_.timeSignature.barAtBeats(positionBeats()); }

void Engine::setStretchPreset(StretchPreset preset) {
    presetCode_.store(static_cast<int>(preset));
    presetEpoch_.fetch_add(1);
}

void Engine::setCacheBudget(std::size_t bytes) { store_->setBudget(bytes); }

void Engine::prepareMixBuffers() {
    const int frames = std::max(scratchFrames_, 1);
    const int buses = std::max(static_cast<int>(project_.channels.size()), 1);
    const int deviceChannels = std::max(deviceChannels_, 2);
    scratch_.assign(static_cast<size_t>(frames) * static_cast<size_t>(deviceChannels), 0.0f);
    buses_.assign(static_cast<size_t>(buses) * 2 * static_cast<size_t>(frames), 0.0f);
    post_.assign(static_cast<size_t>(frames) * 2, 0.0f);
    popped_.assign(static_cast<size_t>(frames) * 2, 0.0f);
    discard_.assign(static_cast<size_t>(frames) * 2, 0.0f);
}

void Engine::noteCallback(std::uint64_t nanoseconds, std::uint32_t frames) {
    callbackNs_.store(nanoseconds);
    callbackFrames_.store(frames == 0 ? 1 : frames);
}

void Engine::setHardwareOutput(int channelIndex, int startChannel, int width) {
    if (channelIndex < 0 || channelIndex >= static_cast<int>(project_.channels.size())) {
        return;
    }
    const int usedWidth = std::max(width, 1);
    hardwareStart_[static_cast<size_t>(channelIndex)].store(startChannel);
    hardwareWidth_[static_cast<size_t>(channelIndex)].store(usedWidth);
    project_.channels[static_cast<size_t>(channelIndex)].hardwareStart = startChannel;
    project_.channels[static_cast<size_t>(channelIndex)].hardwareWidth = usedWidth;
    int needed = 2;
    for (size_t index = 0; index < hardwareStart_.size(); ++index) {
        const int start = hardwareStart_[index].load();
        const int assigned = hardwareWidth_[index].load();
        if (start >= 0) {
            needed = std::max(needed, start + assigned);
        }
    }
    if (!deviceOpen_) {
        deviceChannels_ = std::max(2, needed);
        prepareMixBuffers();
    } else if (needed != deviceChannels_) {
        deviceChannels_ = needed;
        std::string error;
        startDevice(error);
    }
}

void Engine::requestSeek(std::int64_t frame) {
    playhead_.store(std::max<std::int64_t>(frame, 0));
    seekId_.fetch_add(1);
}

int Engine::channelCountFor(const AudioEvent& event) const {
    return std::clamp(event.file.channels, 1, 2);
}

bool Engine::eventNeedsStretch(const AudioEvent& event, double ratio, double transpose) const {
    (void)event;
    return std::abs(ratio - 1.0) > 0.01 || std::abs(transpose) > 0.01;
}

void Engine::ensureVoices() {
    const std::int64_t playhead = playhead_.load();
    const double now = static_cast<double>(playhead) / static_cast<double>(outputRate_);
    const double horizon = now + 0.20;
    int stretching = 0;
    for (const auto& voice : voices_) {
        if (voice->active.load() && voice->stretching.load()) {
            ++stretching;
        }
        voice->active.store(false);
    }
    for (int eventIndex = 0; eventIndex < static_cast<int>(project_.events.size()); ++eventIndex) {
        const AudioEvent& event = project_.events[static_cast<size_t>(eventIndex)];
        if (event.endSecond < now || event.startSecond > horizon) {
            continue;
        }
        Voice* existing = nullptr;
        for (const auto& voice : voices_) {
            if (voice->eventIndex == eventIndex) {
                existing = voice.get();
                break;
            }
        }
        if (existing == nullptr) {
            if (static_cast<int>(voices_.size()) >= 64) {
                continue;
            }
            voices_.push_back(std::make_unique<Voice>());
            existing = voices_.back().get();
            existing->eventIndex = eventIndex;
            existing->channels = channelCountFor(event);
            const int fifoFrames = std::max(outputRate_ * 150 / 1000, kProduceFrames * 2);
            existing->ring.configure(existing->channels, fifoFrames);
            existing->readFrame.store(playhead);
            existing->writeFrame = playhead;
            publishedVoices_.store(static_cast<int>(voices_.size()), std::memory_order_release);
        }
        double source = 0;
        double later = 0;
        const double sampleStart = std::max(now, event.startSecond);
        const double sampleEnd = std::min(horizon, event.endSecond);
        const bool audible = event.sourceAt(project_.tempo, sampleStart, source);
        if (sampleEnd > sampleStart) {
            event.sourceAt(project_.tempo, sampleEnd, later);
        } else {
            later = source;
        }
        const double span = sampleEnd - sampleStart;
        const double ratio = audible && span > 1.0e-4 ? (later - source) / span : 1.0;
        const double transpose = event.automation.empty() ? 0.0 : event.automation.front().transpose.valueAt(0);
        const bool needStretch = eventNeedsStretch(event, ratio, transpose);
        if (needStretch && !existing->stretching && stretching >= kMaxStretchers) {
            continue;
        }
        if (needStretch && !existing->stretching) {
            ++stretching;
        }
        existing->stretching.store(needStretch);
        existing->active.store(true);
    }
}

void Engine::fillAhead() {
    const std::uint64_t seek = seekId_.load();
    if (appliedSeek_.load() != seek) {
        for (const auto& voice : voices_) {
            voice->ring.clear();
            voice->primed = false;
            voice->readFrame.store(playhead_.load());
            voice->writeFrame = playhead_.load();
        }
        appliedSeek_.store(seek);
    }
    ensureVoices();
    const std::int64_t playhead = playhead_.load();
    const int targetAhead = std::max(outputRate_ * 120 / 1000, kProduceFrames);
    const int epoch = presetEpoch_.load();
    for (const auto& voice : voices_) {
        if (!voice->active.load()) {
            continue;
        }
        if (voice->writeFrame + voice->ring.filled() < playhead) {
            voice->ring.clear();
            voice->primed = false;
            voice->writeFrame = playhead;
            voice->readFrame.store(playhead);
        }
        const AudioEvent& event = project_.events[static_cast<size_t>(voice->eventIndex)];
        const std::int64_t eventEnd = static_cast<std::int64_t>(std::llround(event.endSecond * outputRate_));
        while (voice->writeFrame < playhead + targetAhead && voice->writeFrame < eventEnd && voice->ring.space() >= kProduceFrames) {
            const int frames = std::min(kProduceFrames, static_cast<int>(eventEnd - voice->writeFrame));
            const double t0 = static_cast<double>(voice->writeFrame) / static_cast<double>(outputRate_);
            const double t1 = static_cast<double>(voice->writeFrame + frames) / static_cast<double>(outputRate_);
            double sourceStart = 0;
            double sourceEnd = 0;
            if (!event.sourceAt(project_.tempo, t0, sourceStart) || !event.sourceAt(project_.tempo, t1, sourceEnd) ||
                sourceEnd + 1.0e-6 < sourceStart) {
                voice->interleaved.assign(static_cast<size_t>(frames) * static_cast<size_t>(voice->channels), 0.0f);
                voice->ring.push(voice->interleaved.data(), frames);
                voice->writeFrame += frames;
                voice->primed = false;
                continue;
            }
            const double ratio = (sourceEnd - sourceStart) * static_cast<double>(outputRate_) / static_cast<double>(frames);
            double transpose = 0;
            for (const LayerAutomation& automation : event.automation) {
                const double content = event.contentTime(project_.tempo, t0, automation.layer);
                transpose += automation.transpose.valueAt(content);
            }
            const bool needStretch = eventNeedsStretch(event, ratio, transpose);
            voice->stretching.store(needStretch);
            if (!needStretch) {
                voice->primed = false;
            }
            const auto sourceFrame = static_cast<std::int64_t>(std::llround(sourceStart * outputRate_));
            if (!needStretch) {
                voice->interleaved.resize(static_cast<size_t>(frames) * static_cast<size_t>(voice->channels));
                bool cacheMiss = false;
                const bool served = store_->readFrames(event.file, outputRate_, sourceFrame, voice->interleaved.data(), frames,
                                                       voice->channels, cacheMiss);
                if (!served || cacheMiss) {
                    std::fill(voice->interleaved.begin(), voice->interleaved.end(), 0.0f);
                }
                voice->ring.push(voice->interleaved.data(), frames);
                voice->writeFrame += frames;
                continue;
            }
            if (voice->stretch == nullptr || voice->stretch->inputLatency() < 0) {
                voice->stretch = std::make_unique<signalsmith::stretch::SignalsmithStretch<float>>(1);
            }
            if (!voice->primed || epoch != 0) {
                if (preset() == StretchPreset::Default) {
                    voice->stretch->presetDefault(voice->channels, static_cast<float>(outputRate_));
                } else {
                    voice->stretch->presetCheaper(voice->channels, static_cast<float>(outputRate_));
                }
                voice->stretchBytes = estimateStretchBytes(*voice->stretch, voice->channels);
                const double safeRatio = std::clamp(ratio, 0.05, 8.0);
                const int seekLength = std::max(voice->stretch->outputSeekLength(static_cast<float>(safeRatio)), 1);
                voice->interleaved.assign(static_cast<size_t>(seekLength) * static_cast<size_t>(voice->channels), 0.0f);
                bool cacheMiss = false;
                store_->readFrames(event.file, outputRate_, sourceFrame, voice->interleaved.data(), seekLength, voice->channels,
                                   cacheMiss);
                voice->inputPlanar.resize(static_cast<size_t>(seekLength) * static_cast<size_t>(voice->channels));
                interleaveToPlanar(voice->interleaved.data(), voice->inputPlanar.data(), seekLength, voice->channels);
                voice->stretch->setTransposeSemitones(static_cast<float>(transpose));
                Planar input{voice->inputPlanar.data(), seekLength};
                voice->stretch->outputSeek(input, seekLength);
                voice->sourceCursor = static_cast<double>(sourceFrame + seekLength);
                voice->primed = true;
            }
            voice->stretch->setTransposeSemitones(static_cast<float>(transpose));
            const int inputFrames = std::max(1, static_cast<int>(std::llround(std::clamp(ratio, 0.05, 8.0) * frames)));
            voice->interleaved.assign(static_cast<size_t>(inputFrames) * static_cast<size_t>(voice->channels), 0.0f);
            bool cacheMiss = false;
            const auto cursorFrame = static_cast<std::int64_t>(std::llround(voice->sourceCursor));
            store_->readFrames(event.file, outputRate_, cursorFrame, voice->interleaved.data(), inputFrames, voice->channels,
                               cacheMiss);
            voice->inputPlanar.resize(static_cast<size_t>(inputFrames) * static_cast<size_t>(voice->channels));
            voice->outputPlanar.assign(static_cast<size_t>(frames) * static_cast<size_t>(voice->channels), 0.0f);
            interleaveToPlanar(voice->interleaved.data(), voice->inputPlanar.data(), inputFrames, voice->channels);
            Planar input{voice->inputPlanar.data(), inputFrames};
            Planar output{voice->outputPlanar.data(), frames};
            voice->stretch->process(input, inputFrames, output, frames);
            voice->interleaved.resize(static_cast<size_t>(frames) * static_cast<size_t>(voice->channels));
            planarToInterleave(voice->outputPlanar.data(), voice->interleaved.data(), frames, voice->channels);
            if (cacheMiss) {
                const int fade = std::min(frames, 64);
                for (int frame = 0; frame < fade; ++frame) {
                    const float gain = 1.0f - static_cast<float>(frame) / static_cast<float>(fade);
                    for (int channel = 0; channel < voice->channels; ++channel) {
                        voice->interleaved[static_cast<size_t>(frame) * static_cast<size_t>(voice->channels) +
                                           static_cast<size_t>(channel)] *= gain;
                    }
                }
            }
            voice->ring.push(voice->interleaved.data(), frames);
            voice->sourceCursor += inputFrames;
            voice->writeFrame += frames;
        }
    }
    if (epoch != 0) {
        int seen = epoch;
        presetEpoch_.compare_exchange_strong(seen, 0);
    }
}

void Engine::fillLoop() {
    while (!stopThread_.load()) {
        if (playing_.load() || inlineFill_) {
            fillAhead();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(playing_.load() ? 2 : 10));
    }
}

void Engine::mixBlock(float* deviceOut, int frameCount, bool advance) {
    const int busChannels = 2;
    const size_t channelCount = project_.channels.size();
    const int frames = std::max(frameCount, 0);
    if (frames > scratchFrames_ || buses_.size() < channelCount * static_cast<size_t>(busChannels) * static_cast<size_t>(scratchFrames_)) {
        return;
    }
    std::fill_n(buses_.begin(), channelCount * static_cast<size_t>(busChannels) * static_cast<size_t>(frames), 0.0f);
    const bool solo = project_.anySolo();
    const std::int64_t origin = playhead_.load();
    if (appliedSeek_.load() == seekId_.load()) {
        const int voiceCount = publishedVoices_.load(std::memory_order_acquire);
        for (int voiceIndex = 0; voiceIndex < voiceCount; ++voiceIndex) {
            Voice* voice = voices_[static_cast<size_t>(voiceIndex)].get();
            if (!voice->active.load() && voice->ring.filled() == 0) {
                continue;
            }
            const AudioEvent& event = project_.events[static_cast<size_t>(voice->eventIndex)];
            if (event.channelIndex < 0 || event.channelIndex >= static_cast<int>(channelCount)) {
                continue;
            }
            const MixerChannel& channel = project_.channels[static_cast<size_t>(event.channelIndex)];
            if (!channel.audible || (solo && !channel.solo)) {
                continue;
            }
            std::int64_t readFrame = voice->readFrame.load();
            if (readFrame < origin) {
                int drop = static_cast<int>(std::min<std::int64_t>(origin - readFrame, voice->ring.filled()));
                while (drop > 0) {
                    const int chunk = std::min(drop, scratchFrames_);
                    voice->ring.pop(discard_.data(), chunk);
                    drop -= chunk;
                    readFrame += chunk;
                }
                voice->readFrame.store(readFrame);
            }
            if (readFrame > origin) {
                continue;
            }
            const int needed = frames - static_cast<int>(origin - readFrame);
            if (needed <= 0) {
                continue;
            }
            std::fill_n(popped_.begin(), static_cast<size_t>(needed) * static_cast<size_t>(voice->channels), 0.0f);
            const int got = voice->ring.pop(popped_.data(), needed);
            if (got < needed) {
                underruns_.fetch_add(1);
            }
            voice->readFrame.store(readFrame + got);
            const int offset = static_cast<int>(readFrame - origin);
            float* bus = buses_.data() + (static_cast<size_t>(event.channelIndex) * static_cast<size_t>(busChannels) *
                                           static_cast<size_t>(frames));
            for (int frame = 0; frame < got; ++frame) {
                const double arrangement = static_cast<double>(readFrame + frame) / static_cast<double>(outputRate_);
                const double clipPos = arrangement - event.startSecond;
                const double clipRemain = event.endSecond - arrangement;
                float envelope = 1.0f;
                if (event.fadeInSecond > 1.0e-6 && clipPos < event.fadeInSecond) {
                    envelope *= fadeGain(static_cast<float>(clipPos / event.fadeInSecond), event.equalPowerIn);
                }
                if (event.fadeOutSecond > 1.0e-6 && clipRemain < event.fadeOutSecond) {
                    envelope *= fadeGain(static_cast<float>(clipRemain / event.fadeOutSecond), event.equalPowerOut);
                }
                float gain = envelope;
                float pan = 0.5f;
                bool clipPan = false;
                for (const LayerAutomation& automation : event.automation) {
                    const double content = event.contentTime(project_.tempo, arrangement, automation.layer);
                    gain *= static_cast<float>(automation.gain.valueAt(content));
                    if (!automation.pan.points.empty()) {
                        pan = static_cast<float>(automation.pan.valueAt(content));
                        clipPan = true;
                    }
                }
                // Channel pan is the constant-power stage. A clip with no pan curve stays at unity
                // so a centered clip is not attenuated twice.
                float leftGain = gain;
                float rightGain = gain;
                if (clipPan) {
                    const float angle = std::clamp(pan, 0.0f, 1.0f) * 1.5707963f;
                    leftGain = std::cos(angle) * gain;
                    rightGain = std::sin(angle) * gain;
                }
                const int destFrame = offset + frame;
                if (destFrame < 0 || destFrame >= frameCount) {
                    continue;
                }
                const float left = popped_[static_cast<size_t>(frame) * static_cast<size_t>(voice->channels)];
                const float right = voice->channels > 1
                                        ? popped_[static_cast<size_t>(frame) * static_cast<size_t>(voice->channels) + 1]
                                        : left;
                bus[static_cast<size_t>(destFrame)] += left * leftGain;
                bus[static_cast<size_t>(frames) + static_cast<size_t>(destFrame)] += right * rightGain;
            }
        }
    }
    for (int channelIndex : mixOrder_) {
        const MixerChannel& channel = project_.channels[static_cast<size_t>(channelIndex)];
        float* bus = buses_.data() + static_cast<size_t>(channelIndex) * static_cast<size_t>(busChannels) *
                                          static_cast<size_t>(frames);
        const double beats = project_.tempo.beatsAtSeconds(static_cast<double>(origin) / outputRate_);
        const double seconds = static_cast<double>(origin) / outputRate_;
        const double volume = channel.volume.valueAt(beats);
        const double pan = channel.pan.valueAt(beats);
        const bool muted = channel.mute.valueAt(beats) >= 0.5 || channel.mute.valueAt(seconds) >= 0.5;
        const float angle = std::clamp(static_cast<float>(pan), 0.0f, 1.0f) * 1.5707963f;
        const float leftGain = muted ? 0.0f : static_cast<float>(volume) * std::cos(angle);
        const float rightGain = muted ? 0.0f : static_cast<float>(volume) * std::sin(angle);
        for (int frame = 0; frame < frames; ++frame) {
            post_[static_cast<size_t>(frame)] = bus[static_cast<size_t>(frame)] * leftGain;
            post_[static_cast<size_t>(frames) + static_cast<size_t>(frame)] =
                bus[static_cast<size_t>(frames) + static_cast<size_t>(frame)] * rightGain;
        }
        const int hardware = hardwareStart_[static_cast<size_t>(channelIndex)].load();
        const int width = hardwareWidth_[static_cast<size_t>(channelIndex)].load();
        if (hardware >= 0 && deviceOut != nullptr) {
            for (int frame = 0; frame < frames; ++frame) {
                const float left = post_[static_cast<size_t>(frame)];
                const float right = post_[static_cast<size_t>(frames) + static_cast<size_t>(frame)];
                if (width <= 1) {
                    const int outChannel = hardware;
                    if (outChannel >= 0 && outChannel < deviceChannels_) {
                        deviceOut[static_cast<size_t>(frame) * static_cast<size_t>(deviceChannels_) +
                                  static_cast<size_t>(outChannel)] += 0.5f * (left + right);
                    }
                } else {
                    if (hardware < deviceChannels_) {
                        deviceOut[static_cast<size_t>(frame) * static_cast<size_t>(deviceChannels_) +
                                  static_cast<size_t>(hardware)] += left;
                    }
                    if (hardware + 1 < deviceChannels_) {
                        deviceOut[static_cast<size_t>(frame) * static_cast<size_t>(deviceChannels_) +
                                  static_cast<size_t>(hardware + 1)] += right;
                    }
                }
            }
        } else if (channel.destination >= 0 && channel.destination < static_cast<int>(channelCount)) {
            float* destination = buses_.data() + static_cast<size_t>(channel.destination) * static_cast<size_t>(busChannels) *
                                                      static_cast<size_t>(frames);
            for (int frame = 0; frame < frames; ++frame) {
                destination[static_cast<size_t>(frame)] += post_[static_cast<size_t>(frame)];
                destination[static_cast<size_t>(frames) + static_cast<size_t>(frame)] +=
                    post_[static_cast<size_t>(frames) + static_cast<size_t>(frame)];
            }
        }
    }
    if (advance && playing_.load()) {
        playhead_.store(origin + frames);
    }
}

void Engine::pump(float* output, int frameCount) {
    std::fill(output, output + static_cast<size_t>(frameCount) * static_cast<size_t>(deviceChannels_), 0.0f);
    int done = 0;
    while (done < frameCount) {
        const int chunk = std::min(scratchFrames_, frameCount - done);
        mixBlock(output + static_cast<size_t>(done) * static_cast<size_t>(deviceChannels_), chunk, true);
        done += chunk;
    }
}

namespace {

void deviceCallback(ma_device* device, void* output, const void*, ma_uint32 frameCount) {
    auto* engine = static_cast<Engine*>(device->pUserData);
    const auto started = std::chrono::steady_clock::now();
    engine->pump(static_cast<float*>(output), static_cast<int>(frameCount));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    engine->noteCallback(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
                         frameCount);
}

}  // namespace

bool Engine::startDevice(std::string& error) {
    const bool resume = playing_.load();
    playing_.store(false);
    stopThread_.store(true);
    if (fillThread_.joinable()) {
        fillThread_.join();
    }
    stopThread_.store(false);
    stopDevice();
    if (context_ == nullptr) {
        error = "Audio context is unavailable";
        return false;
    }
    auto* device = new ma_device();
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = static_cast<ma_uint32>(deviceChannels_);
    config.sampleRate = static_cast<ma_uint32>(outputRate_);
    config.dataCallback = deviceCallback;
    config.pUserData = this;
    config.periodSizeInFrames = static_cast<ma_uint32>(scratchFrames_);
    ma_device_info* infos = nullptr;
    ma_uint32 playbackCount = 0;
    ma_context_get_devices(static_cast<ma_context*>(context_), &infos, &playbackCount, nullptr, nullptr);
    if (deviceIndex_ >= 0 && static_cast<ma_uint32>(deviceIndex_) < playbackCount) {
        config.playback.pDeviceID = &infos[deviceIndex_].id;
    }
    if (ma_device_init(static_cast<ma_context*>(context_), &config, device) != MA_SUCCESS) {
        delete device;
        error = "Could not open the playback device";
        return false;
    }
    outputRate_ = static_cast<int>(device->sampleRate);
    deviceChannels_ = static_cast<int>(device->playback.channels);
    prepareMixBuffers();
    publishedVoices_.store(0, std::memory_order_release);
    voices_.clear();
    if (ma_device_start(device) != MA_SUCCESS) {
        ma_device_uninit(device);
        delete device;
        error = "Could not start the playback device";
        return false;
    }
    device_ = device;
    deviceOpen_ = true;
    if (resume) {
        play();
    }
    return true;
}

void Engine::stopDevice() {
    if (device_ != nullptr) {
        auto* device = static_cast<ma_device*>(device_);
        ma_device_uninit(device);
        delete device;
        device_ = nullptr;
    }
    deviceOpen_ = false;
}

std::vector<std::string> Engine::playbackDevices() const { return deviceNames_; }

void Engine::selectDevice(int index) {
    deviceIndex_ = index;
    if (deviceOpen_) {
        std::string error;
        startDevice(error);
    }
}

bool Engine::renderToWav(const RenderRequest& request, std::string& error) {
    const double length = project_.lengthSeconds();
    if (!(length > 0)) {
        error = "Project has no arrangement audio";
        return false;
    }
    outputRate_ = std::max(request.sampleRate, 8000);
    int channels = 2;
    if (request.multichannel) {
        channels = 2;
        for (size_t channelIndex = 0; channelIndex < hardwareStart_.size(); ++channelIndex) {
            const int start = hardwareStart_[channelIndex].load();
            const int width = hardwareWidth_[channelIndex].load();
            if (start >= 0) {
                channels = std::max(channels, start + width);
            }
        }
    }
    deviceChannels_ = channels;
    prepareMixBuffers();
    publishedVoices_.store(0, std::memory_order_release);
    voices_.clear();
    playhead_.store(0);
    appliedSeek_.store(seekId_.load());
    ma_encoder encoder;
    const ma_encoder_config config =
        ma_encoder_config_init(ma_encoding_format_wav, toMiniFormat(request.format), static_cast<ma_uint32>(channels),
                               static_cast<ma_uint32>(outputRate_));
    if (ma_encoder_init_file(request.path.c_str(), &config, &encoder) != MA_SUCCESS) {
        error = "Could not create " + request.path;
        return false;
    }
    inlineFill_ = true;
    const auto total = static_cast<std::int64_t>(std::llround(length * outputRate_));
    std::vector<float> block(static_cast<size_t>(scratchFrames_) * static_cast<size_t>(channels));
    std::vector<std::uint8_t> encoded;
    std::int64_t rendered = 0;
    while (rendered < total) {
        const int frames = static_cast<int>(std::min<std::int64_t>(scratchFrames_, total - rendered));
        fillAhead();
        std::fill(block.begin(), block.end(), 0.0f);
        mixBlock(block.data(), frames, false);
        playhead_.store(rendered + frames);
        ma_uint64 written = 0;
        const size_t sampleCount = static_cast<size_t>(frames) * static_cast<size_t>(channels);
        if (request.format == PcmFormat::F32) {
            ma_encoder_write_pcm_frames(&encoder, block.data(), static_cast<ma_uint64>(frames), &written);
        } else if (request.format == PcmFormat::S16) {
            encoded.resize(sampleCount * 2);
            for (size_t sample = 0; sample < sampleCount; ++sample) {
                const int value = static_cast<int>(std::lrintf(std::clamp(block[sample], -1.0f, 1.0f) * 32767.0f));
                const auto quantized = static_cast<std::int16_t>(value);
                std::memcpy(encoded.data() + sample * 2, &quantized, 2);
            }
            ma_encoder_write_pcm_frames(&encoder, encoded.data(), static_cast<ma_uint64>(frames), &written);
        } else if (request.format == PcmFormat::S32) {
            encoded.resize(sampleCount * 4);
            for (size_t sample = 0; sample < sampleCount; ++sample) {
                const auto quantized = static_cast<std::int32_t>(
                    std::lrintf(std::clamp(block[sample], -1.0f, 1.0f) * 2147483647.0f));
                std::memcpy(encoded.data() + sample * 4, &quantized, 4);
            }
            ma_encoder_write_pcm_frames(&encoder, encoded.data(), static_cast<ma_uint64>(frames), &written);
        } else if (request.format == PcmFormat::S24) {
            encoded.resize(sampleCount * 3);
            for (size_t sample = 0; sample < sampleCount; ++sample) {
                const int quantized = static_cast<int>(std::lrintf(std::clamp(block[sample], -1.0f, 1.0f) * 8388607.0f));
                encoded[sample * 3] = static_cast<std::uint8_t>(quantized & 0xFF);
                encoded[sample * 3 + 1] = static_cast<std::uint8_t>((quantized >> 8) & 0xFF);
                encoded[sample * 3 + 2] = static_cast<std::uint8_t>((quantized >> 16) & 0xFF);
            }
            ma_encoder_write_pcm_frames(&encoder, encoded.data(), static_cast<ma_uint64>(frames), &written);
        } else {
            encoded.resize(sampleCount);
            for (size_t sample = 0; sample < sampleCount; ++sample) {
                const int quantized = static_cast<int>(std::lrintf(std::clamp(block[sample], -1.0f, 1.0f) * 127.5f + 128.0f));
                encoded[sample] = static_cast<std::uint8_t>(std::clamp(quantized, 0, 255));
            }
            ma_encoder_write_pcm_frames(&encoder, encoded.data(), static_cast<ma_uint64>(frames), &written);
        }
        rendered += frames;
    }
    inlineFill_ = false;
    ma_encoder_uninit(&encoder);
    return true;
}

MemoryStats Engine::memory() const {
    MemoryStats stats;
    stats.cacheBytes = store_->bytesInUse();
    stats.cacheBudget = store_->budget();
    stats.modelBytes = project_.modelBytes;
    stats.residentBytes = residentBytes();
    const int voiceCount = publishedVoices_.load(std::memory_order_acquire);
    for (int voiceIndex = 0; voiceIndex < voiceCount; ++voiceIndex) {
        const Voice* voice = voices_[static_cast<size_t>(voiceIndex)].get();
        if (!voice->active.load() && voice->stretch == nullptr) {
            continue;
        }
        stats.stretcherBytes += voice->stretchBytes;
        stats.fifoBytes += voice->ring.samples.size() * sizeof(float);
        if (voice->active.load() && voice->stretching.load()) {
            ++stats.activeStretchers;
        }
    }
    return stats;
}

double Engine::cpuPercent() const {
#if defined(__EMSCRIPTEN__)
    return callbackLoad();
#else
    const auto wall = static_cast<std::uint64_t>(timeSeconds() * 1.0e9);
    const std::uint64_t process = processCpuNs();
    const std::uint64_t wallDelta = wall - cpuLastWallNs_;
    if (wallDelta > 0 && process >= cpuLastProcessNs_) {
        cpuPercent_ = 100.0 * static_cast<double>(process - cpuLastProcessNs_) / static_cast<double>(wallDelta);
    }
    cpuLastWallNs_ = wall;
    cpuLastProcessNs_ = process;
    return cpuPercent_;
#endif
}

double Engine::callbackLoad() const {
    const double seconds = static_cast<double>(callbackNs_.load()) / 1.0e9;
    const double buffer = static_cast<double>(callbackFrames_.load()) / static_cast<double>(outputRate_);
    if (buffer <= 0) {
        return 0;
    }
    return 100.0 * seconds / buffer;
}

const char* pcmFormatName(PcmFormat format) {
    switch (format) {
    case PcmFormat::S32:
        return "s32";
    case PcmFormat::S24:
        return "s24";
    case PcmFormat::S16:
        return "s16";
    case PcmFormat::U8:
        return "u8";
    case PcmFormat::F32:
    default:
        return "f32";
    }
}

bool pcmFormatFromName(const std::string& name, PcmFormat& format) {
    if (name == "f32") {
        format = PcmFormat::F32;
    } else if (name == "s32") {
        format = PcmFormat::S32;
    } else if (name == "s24") {
        format = PcmFormat::S24;
    } else if (name == "s16") {
        format = PcmFormat::S16;
    } else if (name == "u8") {
        format = PcmFormat::U8;
    } else {
        return false;
    }
    return true;
}

}  // namespace dawplay
