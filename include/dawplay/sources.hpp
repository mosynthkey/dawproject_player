#pragma once

#include "dawplay/project.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dawplay {

struct MemoryStats {
    size_t cacheBytes = 0;
    size_t cacheBudget = 0;
    size_t stretcherBytes = 0;
    size_t fifoBytes = 0;
    size_t modelBytes = 0;
    size_t residentBytes = 0;
    int activeStretchers = 0;
};

// Shared source audio. Playback and render both read it.
// Uncompressed WAV is read in place. Compressed audio is decoded into a
// bounded int16 block cache.
class AudioStore {
public:
    static constexpr size_t kBlockFrames = 16384;
    static constexpr size_t kDefaultBudget = 8 * 1024 * 1024;

    AudioStore();
    ~AudioStore();

    AudioStore(const AudioStore&) = delete;
    AudioStore& operator=(const AudioStore&) = delete;

    bool openFile(const std::string& path, std::string& error);
    bool openMemory(const std::uint8_t* data, size_t size, const std::string& label, std::string& error);
    std::string projectXml() const;

    void setBudget(size_t bytes);
    size_t budget() const;
    size_t bytesInUse() const;

    // Interleaved float frames at outputRate, beginning at sourceFrame on that rate.
    // Returns false when the cache is full of pinned blocks and this read cannot be served.
    bool readFrames(const FileRef& file, int outputRate, std::int64_t sourceFrame, float* interleaved,
                    int frameCount, int channelCount, bool& cacheMiss);

    std::string archiveLabel() const;

private:
    struct Mapped {
        int fd = -1;
        std::uint8_t* data = nullptr;
        size_t size = 0;
        bool ownedMap = false;
    };
    struct WavView {
        const std::uint8_t* pcm = nullptr;
        size_t pcmBytes = 0;
        int channels = 0;
        int sampleRate = 0;
        int bits = 0;
        bool floating = false;
        std::vector<std::uint8_t> owned;
        std::string tempPath;
        Mapped tempMap;
    };
    struct Decoded {
        std::vector<std::uint8_t> compressed;
        int channels = 0;
        int sampleRate = 0;
        void* decoder = nullptr;
    };
    struct Block {
        std::string key;
        std::int64_t index = 0;
        int rate = 0;
        int channels = 0;
        std::vector<std::int16_t> pcm;
        double anchor = 0;
        bool pinned = false;
    };

    std::string fileKey(const FileRef& file) const;
    bool mapFile(const std::string& path, Mapped& mapped, std::string& error);
    WavView* wavFor(const FileRef& file, std::string& error);
    Decoded* decodedFor(const FileRef& file, std::string& error);
    bool readWav(const WavView& wav, int outputRate, std::int64_t sourceFrame, float* interleaved, int frameCount,
                 int channelCount);
    bool readCached(const FileRef& file, Decoded& decoded, int outputRate, std::int64_t sourceFrame,
                    float* interleaved, int frameCount, int channelCount, bool& cacheMiss);
    void evict(size_t incomingBytes);
    static bool parseWav(const std::uint8_t* data, size_t size, WavView& wav, std::string& error);

    mutable std::mutex mutex_;
    Mapped archive_;
    std::string label_;
    void* zip_ = nullptr;
    std::vector<std::pair<std::string, WavView>> wavs_;
    std::vector<std::pair<std::string, Decoded>> decoded_;
    std::vector<Block> blocks_;
    size_t budget_ = kDefaultBudget;
    size_t used_ = 0;
    std::vector<std::string> tempFiles_;
};

}  // namespace dawplay
