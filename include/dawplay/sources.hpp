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
        Mapped sourceMap;
        const std::uint8_t* bytes = nullptr;
        size_t byteCount = 0;
        int channels = 0;
        int sampleRate = 0;
        void* decoder = nullptr;
    };
    struct Block {
        int fileId = -1;
        std::int64_t index = 0;
        int rate = 0;
        int channels = 0;
        std::vector<std::int16_t> pcm;
        double anchor = 1.0e300;
        double touchedAt = 0;
        bool occupied = false;
    };
    // Deflated WAV on WASM. The zip entry is not random-access, so an inflate cursor
    // pulls only the frames a cache block needs instead of keeping the whole PCM.
    struct ZipWav {
        int entryIndex = -1;
        int fileId = -1;
        void* iter = nullptr;
        std::uint64_t cursor = 0;
        std::uint64_t dataOffset = 0;
        std::uint64_t pcmBytes = 0;
        int channels = 0;
        int sampleRate = 0;
        int bits = 16;
        bool floating = false;
        bool headerReady = false;
    };

    int identify(const FileRef& file);
    bool mapFile(const std::string& path, Mapped& mapped, std::string& error);
    WavView* wavFor(const FileRef& file, std::string& error);
    ZipWav* zipWavFor(const FileRef& file, std::string& error);
    Decoded* decodedFor(const FileRef& file, std::string& error);
    bool readWav(const WavView& wav, int outputRate, std::int64_t sourceFrame, float* interleaved, int frameCount,
                 int channelCount);
    bool readCached(int fileId, Decoded* decoded, ZipWav* zipWav, int outputRate, std::int64_t sourceFrame,
                    float* interleaved, int frameCount, int channelCount, bool& cacheMiss, std::string& error);
    bool fillBlock(Decoded* decoded, ZipWav* zipWav, Block& block, std::string& error);
    void evict(size_t incomingBytes);
    Block* claimBlock(int fileId, std::int64_t index, int rate, int channels, size_t bytes);
    void releaseBlock(Block& block);
    const Block* findBlock(int fileId, int rate, std::int64_t index) const;
    bool zipRead(ZipWav& wav, std::uint64_t offset, std::uint8_t* destination, size_t bytes);
    bool zipPrepare(ZipWav& wav, std::string& error);
    void zipRestart(ZipWav& wav);
    static bool parseWav(const std::uint8_t* data, size_t size, WavView& wav, std::string& error);

    mutable std::mutex mutex_;
    Mapped archive_;
    std::string label_;
    void* zip_ = nullptr;
    std::vector<std::string> keys_;
    std::vector<std::pair<int, WavView>> wavs_;
    std::vector<std::pair<int, Decoded>> decoded_;
    std::vector<ZipWav> zipWavs_;
    std::vector<Block> blocks_;
    std::vector<float> decodeScratch_;
    std::vector<std::uint8_t> byteScratch_;
    size_t budget_ = kDefaultBudget;
    size_t used_ = 0;
    std::vector<std::string> tempFiles_;
};

}  // namespace dawplay
