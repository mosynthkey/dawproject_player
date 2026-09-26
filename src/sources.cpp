#include "dawplay/sources.hpp"

#include "miniaudio.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "miniz.h"
#pragma GCC diagnostic pop

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifndef __EMSCRIPTEN__
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dawplay {
namespace {

std::string lowerExtension(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string extension = path.substr(dot + 1);
    for (char& character : extension) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return extension;
}

bool isWavExtension(const std::string& path) {
    const std::string extension = lowerExtension(path);
    return extension == "wav" || extension == "wave";
}

std::string parentDirectory(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return ".";
    }
    return path.substr(0, slash);
}

float pcmSample(const std::uint8_t* data, int bits, bool floating) {
    if (floating && bits == 32) {
        float value = 0;
        std::memcpy(&value, data, sizeof(float));
        return value;
    }
    if (bits == 16) {
        std::int16_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return static_cast<float>(value) / 32768.0f;
    }
    if (bits == 24) {
        const int value = static_cast<int>(data[0]) | (static_cast<int>(data[1]) << 8) | (static_cast<int>(data[2]) << 16);
        const int signedValue = value & 0x800000 ? value | ~0xFFFFFF : value;
        return static_cast<float>(signedValue) / 8388608.0f;
    }
    if (bits == 32) {
        std::int32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        return static_cast<float>(value) / 2147483648.0f;
    }
    if (bits == 8) {
        return (static_cast<float>(data[0]) - 128.0f) / 128.0f;
    }
    return 0;
}

void writeSilence(float* interleaved, int frameCount, int channelCount) {
    std::fill(interleaved, interleaved + static_cast<size_t>(frameCount) * static_cast<size_t>(channelCount), 0.0f);
}

}  // namespace

AudioStore::AudioStore() = default;

AudioStore::~AudioStore() {
    for (auto& entry : decoded_) {
        if (entry.second.decoder != nullptr) {
            auto* decoder = static_cast<ma_decoder*>(entry.second.decoder);
            ma_decoder_uninit(decoder);
            delete decoder;
            entry.second.decoder = nullptr;
        }
    }
    if (zip_ != nullptr) {
        mz_zip_reader_end(static_cast<mz_zip_archive*>(zip_));
        delete static_cast<mz_zip_archive*>(zip_);
        zip_ = nullptr;
    }
    for (auto& entry : wavs_) {
        if (entry.second.tempMap.data != nullptr && entry.second.tempMap.ownedMap) {
#ifndef __EMSCRIPTEN__
            munmap(entry.second.tempMap.data, entry.second.tempMap.size);
#endif
        }
        if (entry.second.tempMap.fd >= 0) {
#ifndef __EMSCRIPTEN__
            close(entry.second.tempMap.fd);
#endif
        }
    }
    for (const std::string& path : tempFiles_) {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    if (archive_.ownedMap && archive_.data != nullptr) {
#ifndef __EMSCRIPTEN__
        munmap(archive_.data, archive_.size);
#endif
    }
    if (archive_.fd >= 0) {
#ifndef __EMSCRIPTEN__
        close(archive_.fd);
#endif
    }
}

bool AudioStore::parseWav(const std::uint8_t* data, size_t size, WavView& wav, std::string& error) {
    if (size < 44 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        error = "Not a WAV file";
        return false;
    }
    size_t cursor = 12;
    bool foundFormat = false;
    bool foundData = false;
    int audioFormat = 1;
    while (cursor + 8 <= size) {
        char chunkId[5] = {};
        std::memcpy(chunkId, data + cursor, 4);
        const std::uint32_t chunkSize = static_cast<std::uint32_t>(data[cursor + 4]) |
                                        (static_cast<std::uint32_t>(data[cursor + 5]) << 8) |
                                        (static_cast<std::uint32_t>(data[cursor + 6]) << 16) |
                                        (static_cast<std::uint32_t>(data[cursor + 7]) << 24);
        cursor += 8;
        if (cursor + chunkSize > size) {
            break;
        }
        if (std::memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
            audioFormat = data[cursor] | (data[cursor + 1] << 8);
            wav.channels = data[cursor + 2] | (data[cursor + 3] << 8);
            wav.sampleRate = static_cast<int>(data[cursor + 4] | (data[cursor + 5] << 8) | (data[cursor + 6] << 16) |
                                              (data[cursor + 7] << 24));
            wav.bits = data[cursor + 14] | (data[cursor + 15] << 8);
            wav.floating = audioFormat == 3;
            foundFormat = wav.channels > 0 && wav.sampleRate > 0;
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            wav.pcm = data + cursor;
            wav.pcmBytes = chunkSize;
            foundData = true;
        }
        cursor += chunkSize + (chunkSize & 1u);
    }
    if (!foundFormat || !foundData) {
        error = "WAV file is missing fmt or data";
        return false;
    }
    if (audioFormat != 1 && audioFormat != 3) {
        error = "Unsupported WAV encoding";
        return false;
    }
    return true;
}

bool AudioStore::mapFile(const std::string& path, Mapped& mapped, std::string& error) {
#ifndef __EMSCRIPTEN__
    mapped.fd = open(path.c_str(), O_RDONLY);
    if (mapped.fd < 0) {
        error = "Could not open " + path;
        return false;
    }
    struct stat fileStat;
    if (fstat(mapped.fd, &fileStat) != 0) {
        close(mapped.fd);
        mapped.fd = -1;
        error = "Could not stat " + path;
        return false;
    }
    mapped.size = static_cast<size_t>(fileStat.st_size);
    void* view = mmap(nullptr, mapped.size == 0 ? 1 : mapped.size, PROT_READ, MAP_PRIVATE, mapped.fd, 0);
    if (view == MAP_FAILED) {
        close(mapped.fd);
        mapped.fd = -1;
        error = "Could not map " + path;
        return false;
    }
    mapped.data = static_cast<std::uint8_t*>(view);
    mapped.ownedMap = true;
    return true;
#else
    (void)path;
    (void)mapped;
    error = "External files are not available in the web build";
    return false;
#endif
}

bool AudioStore::openFile(const std::string& path, std::string& error) {
    label_ = path;
    if (!mapFile(path, archive_, error)) {
        return false;
    }
    zip_ = new mz_zip_archive();
    mz_zip_zero_struct(static_cast<mz_zip_archive*>(zip_));
    if (!mz_zip_reader_init_mem(static_cast<mz_zip_archive*>(zip_), archive_.data, archive_.size, 0)) {
        error = "Could not read the DAWPROJECT zip";
        return false;
    }
    return true;
}

bool AudioStore::openMemory(const std::uint8_t* data, size_t size, const std::string& label, std::string& error) {
    label_ = label;
    archive_.data = const_cast<std::uint8_t*>(data);
    archive_.size = size;
    archive_.ownedMap = false;
    archive_.fd = -1;
    zip_ = new mz_zip_archive();
    mz_zip_zero_struct(static_cast<mz_zip_archive*>(zip_));
    if (!mz_zip_reader_init_mem(static_cast<mz_zip_archive*>(zip_), archive_.data, archive_.size, 0)) {
        error = "Could not read the DAWPROJECT zip";
        return false;
    }
    return true;
}

std::string AudioStore::projectXml() const {
    if (zip_ == nullptr) {
        return {};
    }
    size_t size = 0;
    void* bytes = mz_zip_reader_extract_file_to_heap(static_cast<mz_zip_archive*>(zip_), "project.xml", &size, 0);
    if (bytes == nullptr) {
        bytes = mz_zip_reader_extract_file_to_heap(static_cast<mz_zip_archive*>(zip_), "Project.xml", &size, 0);
    }
    if (bytes == nullptr) {
        return {};
    }
    std::string xml(static_cast<char*>(bytes), size);
    mz_free(bytes);
    return xml;
}

void AudioStore::setBudget(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    // One stereo block has to fit, or a miss would be reported for every read.
    budget_ = std::max(bytes, kBlockFrames * 2 * sizeof(std::int16_t));
    evict(0);
}

size_t AudioStore::budget() const {
    return budget_;
}

size_t AudioStore::bytesInUse() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return used_;
}

std::string AudioStore::archiveLabel() const {
    return label_;
}

std::string AudioStore::fileKey(const FileRef& file) const {
    return (file.external ? "external:" : "zip:") + file.path;
}

void AudioStore::evict(size_t incomingBytes) {
    while (used_ + incomingBytes > budget_) {
        size_t victim = blocks_.size();
        double farthest = -1;
        for (size_t blockIndex = 0; blockIndex < blocks_.size(); ++blockIndex) {
            if (blocks_[blockIndex].pinned) {
                continue;
            }
            if (blocks_[blockIndex].anchor > farthest) {
                farthest = blocks_[blockIndex].anchor;
                victim = blockIndex;
            }
        }
        if (victim == blocks_.size()) {
            return;
        }
        used_ -= blocks_[victim].pcm.size() * sizeof(std::int16_t);
        blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(victim));
    }
}

namespace {

int locateEntry(mz_zip_archive* zip, const std::string& path) {
    int index = mz_zip_reader_locate_file(zip, path.c_str(), nullptr, 0);
    if (index >= 0) {
        return index;
    }
    std::string swapped = path;
    for (char& character : swapped) {
        if (character == '\\') {
            character = '/';
        }
    }
    return mz_zip_reader_locate_file(zip, swapped.c_str(), nullptr, 0);
}

}  // namespace

AudioStore::WavView* AudioStore::wavFor(const FileRef& file, std::string& error) {
    const std::string key = fileKey(file);
    for (auto& entry : wavs_) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    auto* zip = static_cast<mz_zip_archive*>(zip_);
    if (file.external) {
#ifndef __EMSCRIPTEN__
        std::string path = file.path;
        if (!(path.size() > 0 && path[0] == '/') && !(path.size() > 1 && path[1] == ':')) {
            path = parentDirectory(label_) + "/" + file.path;
        }
        WavView view;
        if (!mapFile(path, view.tempMap, error)) {
            return nullptr;
        }
        if (!parseWav(view.tempMap.data, view.tempMap.size, view, error)) {
            return nullptr;
        }
        wavs_.emplace_back(key, std::move(view));
        return &wavs_.back().second;
#else
        error = "External media is ignored in the web build: " + file.path;
        return nullptr;
#endif
    }
    if (zip == nullptr) {
        error = "No archive is open";
        return nullptr;
    }
    const int index = locateEntry(zip, file.path);
    if (index < 0) {
        error = "Missing media " + file.path;
        return nullptr;
    }
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(zip, static_cast<mz_uint>(index), &stat)) {
        error = "Could not stat " + file.path;
        return nullptr;
    }
    const bool stored = stat.m_method == 0;
    // Only WAV entries are range-read. Anything else, including a WAV payload with
    // a non-WAV name, goes through the decoder and the block cache.
    if (!isWavExtension(file.path)) {
        return nullptr;
    }
    const std::uint8_t* payload = nullptr;
    size_t payloadSize = static_cast<size_t>(stat.m_uncomp_size);
    if (stored && archive_.data != nullptr) {
        const std::uint8_t* local = archive_.data + stat.m_local_header_ofs;
        if (stat.m_local_header_ofs + 30 > archive_.size) {
            error = "Truncated zip local header";
            return nullptr;
        }
        const std::uint16_t nameLength = static_cast<std::uint16_t>(local[26] | (local[27] << 8));
        const std::uint16_t extraLength = static_cast<std::uint16_t>(local[28] | (local[29] << 8));
        const size_t dataOffset = static_cast<size_t>(stat.m_local_header_ofs + 30 + nameLength + extraLength);
        if (dataOffset + static_cast<size_t>(stat.m_comp_size) > archive_.size) {
            error = "Truncated stored zip entry";
            return nullptr;
        }
        payload = archive_.data + dataOffset;
        payloadSize = static_cast<size_t>(stat.m_comp_size);
    } else {
#ifndef __EMSCRIPTEN__
        const std::string tempPath = (std::filesystem::temp_directory_path() /
                                      ("dawplay-" + std::to_string(index) + "-" + std::to_string(payloadSize) + ".wav"))
                                         .string();
        if (!mz_zip_reader_extract_to_file(zip, static_cast<mz_uint>(index), tempPath.c_str(), 0)) {
            error = "Could not inflate " + file.path;
            return nullptr;
        }
        tempFiles_.push_back(tempPath);
        WavView view;
        view.tempPath = tempPath;
        if (!mapFile(tempPath, view.tempMap, error) || !parseWav(view.tempMap.data, view.tempMap.size, view, error)) {
            return nullptr;
        }
        wavs_.emplace_back(key, std::move(view));
        return &wavs_.back().second;
#else
        size_t heapSize = 0;
        void* heap = mz_zip_reader_extract_to_heap(zip, static_cast<mz_uint>(index), &heapSize, 0);
        if (heap == nullptr) {
            error = "Could not inflate " + file.path;
            return nullptr;
        }
        WavView view;
        view.owned.assign(static_cast<std::uint8_t*>(heap), static_cast<std::uint8_t*>(heap) + heapSize);
        mz_free(heap);
        if (!parseWav(view.owned.data(), view.owned.size(), view, error)) {
            return nullptr;
        }
        view.pcm = view.owned.data() + (view.pcm - view.owned.data());
        wavs_.emplace_back(key, std::move(view));
        WavView& storedView = wavs_.back().second;
        // parseWav pointed pcm at the temporary owned buffer address before the move.
        if (!parseWav(storedView.owned.data(), storedView.owned.size(), storedView, error)) {
            return nullptr;
        }
        return &storedView;
#endif
    }
    WavView view;
    if (!parseWav(payload, payloadSize, view, error)) {
        return nullptr;
    }
    wavs_.emplace_back(key, std::move(view));
    return &wavs_.back().second;
}

AudioStore::Decoded* AudioStore::decodedFor(const FileRef& file, std::string& error) {
    const std::string key = fileKey(file);
    for (auto& entry : decoded_) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    auto* zip = static_cast<mz_zip_archive*>(zip_);
    std::vector<std::uint8_t> bytes;
    if (file.external) {
#ifndef __EMSCRIPTEN__
        std::string path = file.path;
        if (!path.empty() && path[0] != '/') {
            path = parentDirectory(label_) + "/" + file.path;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "Could not open " + path;
            return nullptr;
        }
        input.seekg(0, std::ios::end);
        const auto length = input.tellg();
        input.seekg(0);
        bytes.resize(static_cast<size_t>(length));
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
#else
        error = "External media is ignored in the web build";
        return nullptr;
#endif
    } else {
        if (zip == nullptr) {
            return nullptr;
        }
        const int index = locateEntry(zip, file.path);
        if (index < 0) {
            error = "Missing media " + file.path;
            return nullptr;
        }
        size_t heapSize = 0;
        void* heap = mz_zip_reader_extract_to_heap(zip, static_cast<mz_uint>(index), &heapSize, 0);
        if (heap == nullptr) {
            error = "Could not read " + file.path;
            return nullptr;
        }
        bytes.assign(static_cast<std::uint8_t*>(heap), static_cast<std::uint8_t*>(heap) + heapSize);
        mz_free(heap);
    }
    auto* decoder = new ma_decoder();
    const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (ma_decoder_init_memory(bytes.data(), bytes.size(), &config, decoder) != MA_SUCCESS) {
        delete decoder;
        error = "Could not decode " + file.path;
        return nullptr;
    }
    Decoded decoded;
    // The decoder keeps this pointer. Moving the vector transfers the same allocation.
    decoded.compressed = std::move(bytes);
    decoded.channels = static_cast<int>(decoder->outputChannels);
    decoded.sampleRate = static_cast<int>(decoder->outputSampleRate);
    decoded.decoder = decoder;
    decoded_.emplace_back(key, std::move(decoded));
    return &decoded_.back().second;
}

bool AudioStore::readWav(const WavView& wav, int outputRate, std::int64_t sourceFrame, float* interleaved, int frameCount,
                         int channelCount) {
    const int fileChannels = std::max(wav.channels, 1);
    const int bytesPerSample = std::max(wav.bits / 8, 1);
    const int frameBytes = bytesPerSample * fileChannels;
    const std::int64_t fileFrames = frameBytes > 0 ? static_cast<std::int64_t>(wav.pcmBytes / static_cast<size_t>(frameBytes)) : 0;
    const int usedChannels = std::min(channelCount, 2);
    (void)usedChannels;
    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        const double filePosition = static_cast<double>(sourceFrame + frameIndex) * static_cast<double>(wav.sampleRate) /
                                    static_cast<double>(std::max(outputRate, 1));
        const auto leftFrame = static_cast<std::int64_t>(std::floor(filePosition));
        const float fraction = static_cast<float>(filePosition - static_cast<double>(leftFrame));
        auto sample = [&](std::int64_t frame, int channel) {
            if (frame < 0 || frame >= fileFrames || channel >= fileChannels) {
                return 0.0f;
            }
            return pcmSample(wav.pcm + static_cast<size_t>(frame * frameBytes + channel * bytesPerSample), wav.bits, wav.floating);
        };
        for (int channel = 0; channel < channelCount; ++channel) {
            const int sourceChannel = fileChannels == 1 ? 0 : std::min(channel, fileChannels - 1);
            const float first = sample(leftFrame, sourceChannel);
            const float second = sample(leftFrame + 1, sourceChannel);
            interleaved[frameIndex * channelCount + channel] = first + (second - first) * fraction;
        }
    }
    return true;
}

bool AudioStore::readCached(const FileRef& file, Decoded& decoded, int outputRate, std::int64_t sourceFrame,
                            float* interleaved, int frameCount, int channelCount, bool& cacheMiss) {
    cacheMiss = false;
    const int storedChannels = std::clamp(decoded.channels, 1, 2);
    const std::string key = fileKey(file);
    const std::int64_t firstBlock = sourceFrame / static_cast<std::int64_t>(kBlockFrames);
    const std::int64_t lastBlock = (sourceFrame + frameCount - 1) / static_cast<std::int64_t>(kBlockFrames);
    for (Block& block : blocks_) {
        if (block.key == key && block.rate == outputRate) {
            const double distance = std::abs(static_cast<double>(block.index - firstBlock));
            block.anchor = distance;
            block.pinned = block.index >= firstBlock && block.index <= lastBlock;
        }
    }
    for (std::int64_t blockIndex = firstBlock; blockIndex <= lastBlock; ++blockIndex) {
        const auto found = std::find_if(blocks_.begin(), blocks_.end(), [&](const Block& block) {
            return block.key == key && block.rate == outputRate && block.index == blockIndex && block.channels == storedChannels;
        });
        if (found != blocks_.end()) {
            found->pinned = true;
            continue;
        }
        Block block;
        block.key = key;
        block.index = blockIndex;
        block.rate = outputRate;
        block.channels = storedChannels;
        block.pinned = true;
        block.anchor = 0;
        block.pcm.assign(kBlockFrames * static_cast<size_t>(storedChannels), 0);
        const size_t incoming = block.pcm.size() * sizeof(std::int16_t);
        evict(incoming);
        if (used_ + incoming > budget_) {
            cacheMiss = true;
            for (Block& existing : blocks_) {
                existing.pinned = false;
            }
            return false;
        }
        auto* decoder = static_cast<ma_decoder*>(decoded.decoder);
        const double blockSecond = static_cast<double>(blockIndex * static_cast<std::int64_t>(kBlockFrames)) /
                                   static_cast<double>(outputRate);
        const auto fileFrame = static_cast<ma_uint64>(std::max(0.0, blockSecond * decoded.sampleRate));
        const int fileCount = static_cast<int>(std::ceil(static_cast<double>(kBlockFrames) * decoded.sampleRate /
                                                         static_cast<double>(outputRate))) +
                              4;
        std::vector<float> filePcm(static_cast<size_t>(fileCount) * static_cast<size_t>(decoded.channels), 0.0f);
        ma_decoder_seek_to_pcm_frame(decoder, fileFrame);
        ma_uint64 framesRead = 0;
        ma_decoder_read_pcm_frames(decoder, filePcm.data(), static_cast<ma_uint64>(fileCount), &framesRead);
        for (size_t outFrame = 0; outFrame < kBlockFrames; ++outFrame) {
            const double position = static_cast<double>(outFrame) * static_cast<double>(decoded.sampleRate) /
                                    static_cast<double>(outputRate);
            const auto left = static_cast<size_t>(std::min<double>(position, std::max(0, fileCount - 1)));
            const size_t right = std::min(left + 1, static_cast<size_t>(std::max(fileCount - 1, 0)));
            const float fraction = static_cast<float>(position - static_cast<double>(left));
            for (int channel = 0; channel < storedChannels; ++channel) {
                const int sourceChannel = decoded.channels == 1 ? 0 : std::min(channel, decoded.channels - 1);
                const float first = filePcm[left * static_cast<size_t>(decoded.channels) + static_cast<size_t>(sourceChannel)];
                const float second = filePcm[right * static_cast<size_t>(decoded.channels) + static_cast<size_t>(sourceChannel)];
                const float mixed = first + (second - first) * fraction;
                const int quantized = static_cast<int>(std::lrintf(std::clamp(mixed, -1.0f, 1.0f) * 32767.0f));
                block.pcm[outFrame * static_cast<size_t>(storedChannels) + static_cast<size_t>(channel)] =
                    static_cast<std::int16_t>(quantized);
            }
        }
        used_ += incoming;
        blocks_.push_back(std::move(block));
    }
    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        const std::int64_t absolute = sourceFrame + frameIndex;
        const std::int64_t blockIndex = absolute / static_cast<std::int64_t>(kBlockFrames);
        const size_t offset = static_cast<size_t>(absolute % static_cast<std::int64_t>(kBlockFrames));
        const Block* block = nullptr;
        for (const Block& candidate : blocks_) {
            if (candidate.key == key && candidate.rate == outputRate && candidate.index == blockIndex) {
                block = &candidate;
                break;
            }
        }
        for (int channel = 0; channel < channelCount; ++channel) {
            float sample = 0;
            if (block != nullptr) {
                const int sourceChannel = storedChannels == 1 ? 0 : std::min(channel, storedChannels - 1);
                sample = static_cast<float>(block->pcm[offset * static_cast<size_t>(storedChannels) +
                                                       static_cast<size_t>(sourceChannel)]) /
                         32767.0f;
            }
            interleaved[frameIndex * channelCount + channel] = sample;
        }
    }
    for (Block& block : blocks_) {
        block.pinned = false;
    }
    return true;
}

bool AudioStore::readFrames(const FileRef& file, int outputRate, std::int64_t sourceFrame, float* interleaved, int frameCount,
                            int channelCount, bool& cacheMiss) {
    std::lock_guard<std::mutex> lock(mutex_);
    cacheMiss = false;
    if (frameCount <= 0 || channelCount <= 0) {
        return true;
    }
    std::string error;
    if (WavView* wav = wavFor(file, error)) {
        return readWav(*wav, outputRate, sourceFrame, interleaved, frameCount, channelCount);
    }
    if (Decoded* decoded = decodedFor(file, error)) {
        return readCached(file, *decoded, outputRate, sourceFrame, interleaved, frameCount, channelCount, cacheMiss);
    }
    writeSilence(interleaved, frameCount, channelCount);
    return true;
}

}  // namespace dawplay
