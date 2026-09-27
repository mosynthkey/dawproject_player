#include "dawplay/sources.hpp"

#include "miniaudio.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "miniz.h"
#pragma GCC diagnostic pop

#include <algorithm>
#include <chrono>
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
        if (entry.second.sourceMap.data != nullptr && entry.second.sourceMap.ownedMap) {
#ifndef __EMSCRIPTEN__
            munmap(entry.second.sourceMap.data, entry.second.sourceMap.size);
#endif
        }
        if (entry.second.sourceMap.fd >= 0) {
#ifndef __EMSCRIPTEN__
            close(entry.second.sourceMap.fd);
#endif
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
    for (ZipWav& wav : zipWavs_) {
        if (wav.iter != nullptr) {
            mz_zip_reader_extract_iter_free(static_cast<mz_zip_reader_extract_iter_state*>(wav.iter));
            wav.iter = nullptr;
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

int AudioStore::identify(const FileRef& file) {
    if (file.storeId >= 0 && static_cast<size_t>(file.storeId) < keys_.size()) {
        return file.storeId;
    }
    const std::string key = (file.external ? std::string("external:") : std::string("zip:")) + file.path;
    for (int index = 0; index < static_cast<int>(keys_.size()); ++index) {
        if (keys_[static_cast<size_t>(index)] == key) {
            file.storeId = index;
            return index;
        }
    }
    file.storeId = static_cast<int>(keys_.size());
    keys_.push_back(key);
    return file.storeId;
}

void AudioStore::releaseBlock(Block& block) {
    if (!block.occupied) {
        return;
    }
    used_ -= block.pcm.size() * sizeof(std::int16_t);
    block.occupied = false;
    block.fileId = -1;
    block.anchor = 1.0e300;
    block.touchedAt = 0;
}

void AudioStore::evict(size_t incomingBytes) {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    while (used_ + incomingBytes > budget_) {
        size_t victim = blocks_.size();
        double farthest = -1;
        for (size_t blockIndex = 0; blockIndex < blocks_.size(); ++blockIndex) {
            Block& block = blocks_[blockIndex];
            if (!block.occupied) {
                continue;
            }
            // Touched inside the look-ahead window. Two engines share this cache, so a
            // block stays pinned until it ages out instead of being cleared per read.
            if (now - block.touchedAt < 0.25) {
                continue;
            }
            if (block.anchor > farthest) {
                farthest = block.anchor;
                victim = blockIndex;
            }
        }
        if (victim == blocks_.size()) {
            return;
        }
        releaseBlock(blocks_[victim]);
    }
}

AudioStore::Block* AudioStore::claimBlock(int fileId, std::int64_t index, int rate, int channels, size_t bytes) {
    evict(bytes);
    if (used_ + bytes > budget_) {
        return nullptr;
    }
    Block* block = nullptr;
    for (Block& candidate : blocks_) {
        if (!candidate.occupied) {
            block = &candidate;
            break;
        }
    }
    if (block == nullptr) {
        blocks_.emplace_back();
        block = &blocks_.back();
    }
    block->pcm.resize(bytes / sizeof(std::int16_t));
    block->fileId = fileId;
    block->index = index;
    block->rate = rate;
    block->channels = channels;
    block->occupied = true;
    block->anchor = 0;
    block->touchedAt = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    used_ += block->pcm.size() * sizeof(std::int16_t);
    return block;
}

const AudioStore::Block* AudioStore::findBlock(int fileId, int rate, std::int64_t index) const {
    for (const Block& block : blocks_) {
        if (block.occupied && block.fileId == fileId && block.rate == rate && block.index == index) {
            return &block;
        }
    }
    return nullptr;
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
    const int id = identify(file);
    for (auto& entry : wavs_) {
        if (entry.first == id) {
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
        wavs_.emplace_back(id, std::move(view));
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
        wavs_.emplace_back(id, std::move(view));
        return &wavs_.back().second;
#else
        // No temp file in the web build. The block cache streams this entry.
        (void)index;
        return nullptr;
#endif
    }
    WavView view;
    if (!parseWav(payload, payloadSize, view, error)) {
        return nullptr;
    }
    wavs_.emplace_back(id, std::move(view));
    return &wavs_.back().second;
}

void AudioStore::zipRestart(ZipWav& wav) {
    if (wav.iter != nullptr) {
        mz_zip_reader_extract_iter_free(static_cast<mz_zip_reader_extract_iter_state*>(wav.iter));
        wav.iter = nullptr;
    }
    auto* zip = static_cast<mz_zip_archive*>(zip_);
    if (zip == nullptr || wav.entryIndex < 0) {
        return;
    }
    wav.iter = mz_zip_reader_extract_iter_new(zip, static_cast<mz_uint>(wav.entryIndex), 0);
    wav.cursor = 0;
}

bool AudioStore::zipRead(ZipWav& wav, std::uint64_t offset, std::uint8_t* destination, size_t bytes) {
    if (wav.iter == nullptr || wav.cursor > offset) {
        zipRestart(wav);
    }
    auto* iter = static_cast<mz_zip_reader_extract_iter_state*>(wav.iter);
    if (iter == nullptr) {
        return false;
    }
    std::uint8_t discard[4096];
    while (wav.cursor < offset) {
        const size_t skip = static_cast<size_t>(std::min<std::uint64_t>(offset - wav.cursor, sizeof(discard)));
        const size_t got = mz_zip_reader_extract_iter_read(iter, discard, skip);
        if (got == 0) {
            return false;
        }
        wav.cursor += got;
    }
    size_t filled = 0;
    while (filled < bytes) {
        const size_t got = mz_zip_reader_extract_iter_read(iter, destination + filled, bytes - filled);
        if (got == 0) {
            return false;
        }
        filled += got;
        wav.cursor += got;
    }
    return true;
}

bool AudioStore::zipPrepare(ZipWav& wav, std::string& error) {
    if (wav.headerReady) {
        return true;
    }
    zipRestart(wav);
    std::uint8_t lead[12];
    if (!zipRead(wav, 0, lead, sizeof(lead)) || std::memcmp(lead, "RIFF", 4) != 0 || std::memcmp(lead + 8, "WAVE", 4) != 0) {
        error = "Deflated entry is not a WAV file";
        return false;
    }
    auto skipBytes = [&](std::uint64_t count) {
        std::uint8_t discard[4096];
        while (count > 0) {
            const size_t step = static_cast<size_t>(std::min<std::uint64_t>(count, sizeof(discard)));
            if (!zipRead(wav, wav.cursor, discard, step)) {
                return false;
            }
            count -= step;
        }
        return true;
    };
    bool foundFormat = false;
    while (true) {
        std::uint8_t chunk[8];
        if (!zipRead(wav, wav.cursor, chunk, sizeof(chunk))) {
            error = "Deflated WAV ended before the data chunk";
            return false;
        }
        const std::uint32_t chunkSize = static_cast<std::uint32_t>(chunk[4]) | (static_cast<std::uint32_t>(chunk[5]) << 8) |
                                        (static_cast<std::uint32_t>(chunk[6]) << 16) | (static_cast<std::uint32_t>(chunk[7]) << 24);
        const bool dataChunk = std::memcmp(chunk, "data", 4) == 0;
        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
            std::uint8_t format[16];
            if (!zipRead(wav, wav.cursor, format, sizeof(format))) {
                return false;
            }
            const int audioFormat = format[0] | (format[1] << 8);
            wav.channels = format[2] | (format[3] << 8);
            wav.sampleRate = static_cast<int>(format[4] | (format[5] << 8) | (format[6] << 16) | (format[7] << 24));
            wav.bits = format[14] | (format[15] << 8);
            wav.floating = audioFormat == 3;
            foundFormat = wav.channels > 0 && wav.sampleRate > 0;
            if (!skipBytes(chunkSize - 16)) {
                return false;
            }
        } else if (dataChunk) {
            if (!foundFormat) {
                error = "Deflated WAV is missing fmt";
                return false;
            }
            wav.dataOffset = wav.cursor;
            wav.pcmBytes = chunkSize;
            wav.headerReady = true;
            return true;
        } else if (!skipBytes(chunkSize)) {
            return false;
        }
        if (!dataChunk && (chunkSize & 1u) != 0 && !skipBytes(1)) {
            return false;
        }
    }
}

AudioStore::ZipWav* AudioStore::zipWavFor(const FileRef& file, std::string& error) {
#if !defined(__EMSCRIPTEN__)
    (void)file;
    (void)error;
    return nullptr;
#else
    if (file.external || !isWavExtension(file.path)) {
        return nullptr;
    }
    const int id = identify(file);
    for (ZipWav& existing : zipWavs_) {
        if (existing.fileId == id) {
            return &existing;
        }
    }
    auto* zip = static_cast<mz_zip_archive*>(zip_);
    if (zip == nullptr) {
        return nullptr;
    }
    const int index = locateEntry(zip, file.path);
    if (index < 0) {
        return nullptr;
    }
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(zip, static_cast<mz_uint>(index), &stat) || stat.m_method == 0) {
        return nullptr;
    }
    ZipWav created;
    created.entryIndex = index;
    created.fileId = id;
    if (!zipPrepare(created, error)) {
        zipRestart(created);
        if (created.iter != nullptr) {
            mz_zip_reader_extract_iter_free(static_cast<mz_zip_reader_extract_iter_state*>(created.iter));
            created.iter = nullptr;
        }
        return nullptr;
    }
    zipWavs_.push_back(std::move(created));
    return &zipWavs_.back();
#endif
}

AudioStore::Decoded* AudioStore::decodedFor(const FileRef& file, std::string& error) {
    const int id = identify(file);
    for (auto& entry : decoded_) {
        if (entry.first == id) {
            return &entry.second;
        }
    }
    Decoded decoded;
    const std::uint8_t* source = nullptr;
    size_t sourceSize = 0;
    auto releaseMap = [&decoded]() {
#ifndef __EMSCRIPTEN__
        if (decoded.sourceMap.data != nullptr && decoded.sourceMap.ownedMap) {
            munmap(decoded.sourceMap.data, decoded.sourceMap.size);
            decoded.sourceMap.data = nullptr;
        }
        if (decoded.sourceMap.fd >= 0) {
            close(decoded.sourceMap.fd);
            decoded.sourceMap.fd = -1;
        }
#else
        (void)decoded;
#endif
    };
    if (file.external) {
#ifndef __EMSCRIPTEN__
        std::string path = file.path;
        if (!(path.size() > 0 && path[0] == '/') && !(path.size() > 1 && path[1] == ':')) {
            path = parentDirectory(label_) + "/" + file.path;
        }
        if (!mapFile(path, decoded.sourceMap, error)) {
            return nullptr;
        }
        source = decoded.sourceMap.data;
        sourceSize = decoded.sourceMap.size;
#else
        error = "External media is ignored in the web build";
        return nullptr;
#endif
    } else {
        auto* zip = static_cast<mz_zip_archive*>(zip_);
        if (zip == nullptr) {
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
        if (stat.m_method == 0 && archive_.data != nullptr) {
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
            source = archive_.data + dataOffset;
            sourceSize = static_cast<size_t>(stat.m_comp_size);
        } else {
            size_t heapSize = 0;
            void* heap = mz_zip_reader_extract_to_heap(zip, static_cast<mz_uint>(index), &heapSize, 0);
            if (heap == nullptr) {
                error = "Could not read " + file.path;
                return nullptr;
            }
            decoded.compressed.assign(static_cast<std::uint8_t*>(heap), static_cast<std::uint8_t*>(heap) + heapSize);
            mz_free(heap);
            source = decoded.compressed.data();
            sourceSize = decoded.compressed.size();
        }
    }
    auto* decoder = new ma_decoder();
    const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (source == nullptr || ma_decoder_init_memory(source, sourceSize, &config, decoder) != MA_SUCCESS) {
        delete decoder;
        releaseMap();
        error = "Could not decode " + file.path;
        return nullptr;
    }
    decoded.bytes = source;
    decoded.byteCount = sourceSize;
    decoded.channels = static_cast<int>(decoder->outputChannels);
    decoded.sampleRate = static_cast<int>(decoder->outputSampleRate);
    decoded.decoder = decoder;
    decoded_.emplace_back(id, std::move(decoded));
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

bool AudioStore::fillBlock(Decoded* decoded, ZipWav* zipWav, Block& block, std::string& error) {
    const int fileRate = std::max(decoded != nullptr ? decoded->sampleRate : zipWav->sampleRate, 1);
    const int fileChannels = std::max(decoded != nullptr ? decoded->channels : zipWav->channels, 1);
    const int storedChannels = std::max(block.channels, 1);
    const double blockSecond = static_cast<double>(block.index * static_cast<std::int64_t>(kBlockFrames)) /
                               static_cast<double>(std::max(block.rate, 1));
    const auto fileFrame = static_cast<std::uint64_t>(std::max(0.0, blockSecond * fileRate));
    const int fileCount = static_cast<int>(std::ceil(static_cast<double>(kBlockFrames) * fileRate /
                                                     static_cast<double>(std::max(block.rate, 1)))) +
                          4;
    const size_t scratchFrames = static_cast<size_t>(fileCount) * static_cast<size_t>(fileChannels);
    if (decodeScratch_.size() < scratchFrames) {
        decodeScratch_.resize(scratchFrames);
    }
    std::fill_n(decodeScratch_.begin(), scratchFrames, 0.0f);
    if (decoded != nullptr) {
        auto* decoder = static_cast<ma_decoder*>(decoded->decoder);
        ma_decoder_seek_to_pcm_frame(decoder, static_cast<ma_uint64>(fileFrame));
        ma_uint64 framesRead = 0;
        ma_decoder_read_pcm_frames(decoder, decodeScratch_.data(), static_cast<ma_uint64>(fileCount), &framesRead);
        (void)framesRead;
    } else {
        const int bytesPerSample = std::max(zipWav->bits / 8, 1);
        const int frameBytes = bytesPerSample * fileChannels;
        const std::uint64_t pcmFrames = frameBytes > 0 ? zipWav->pcmBytes / static_cast<std::uint64_t>(frameBytes) : 0;
        const size_t byteCount = static_cast<size_t>(fileCount) * static_cast<size_t>(frameBytes);
        if (byteScratch_.size() < byteCount) {
            byteScratch_.resize(byteCount);
        }
        std::fill(byteScratch_.begin(), byteScratch_.begin() + static_cast<std::ptrdiff_t>(byteCount), 0);
        if (fileFrame < pcmFrames) {
            const std::uint64_t offset = zipWav->dataOffset + fileFrame * static_cast<std::uint64_t>(frameBytes);
            const size_t available = static_cast<size_t>(std::min<std::uint64_t>(
                pcmFrames - fileFrame, static_cast<std::uint64_t>(fileCount)));
            if (!zipRead(*zipWav, offset, byteScratch_.data(), available * static_cast<size_t>(frameBytes))) {
                error = "Could not read deflated WAV";
                return false;
            }
        }
        for (int frame = 0; frame < fileCount; ++frame) {
            for (int channel = 0; channel < fileChannels; ++channel) {
                const std::uint8_t* sample = byteScratch_.data() + static_cast<size_t>(frame * frameBytes + channel * bytesPerSample);
                decodeScratch_[static_cast<size_t>(frame) * static_cast<size_t>(fileChannels) + static_cast<size_t>(channel)] =
                    pcmSample(sample, zipWav->bits, zipWav->floating);
            }
        }
    }
    for (size_t outFrame = 0; outFrame < kBlockFrames; ++outFrame) {
        const double position = static_cast<double>(outFrame) * static_cast<double>(fileRate) /
                                static_cast<double>(std::max(block.rate, 1));
        const auto left = static_cast<size_t>(std::min<double>(position, std::max(0, fileCount - 1)));
        const size_t right = std::min(left + 1, static_cast<size_t>(std::max(fileCount - 1, 0)));
        const float fraction = static_cast<float>(position - static_cast<double>(left));
        for (int channel = 0; channel < storedChannels; ++channel) {
            const int sourceChannel = fileChannels == 1 ? 0 : std::min(channel, fileChannels - 1);
            const float first = decodeScratch_[left * static_cast<size_t>(fileChannels) + static_cast<size_t>(sourceChannel)];
            const float second = decodeScratch_[right * static_cast<size_t>(fileChannels) + static_cast<size_t>(sourceChannel)];
            const float mixed = first + (second - first) * fraction;
            const int quantized = static_cast<int>(std::lrintf(std::clamp(mixed, -1.0f, 1.0f) * 32767.0f));
            block.pcm[outFrame * static_cast<size_t>(storedChannels) + static_cast<size_t>(channel)] =
                static_cast<std::int16_t>(quantized);
        }
    }
    return true;
}

bool AudioStore::readCached(int fileId, Decoded* decoded, ZipWav* zipWav, int outputRate, std::int64_t sourceFrame,
                            float* interleaved, int frameCount, int channelCount, bool& cacheMiss, std::string& error) {
    cacheMiss = false;
    const int fileChannels = decoded != nullptr ? decoded->channels : zipWav->channels;
    const int storedChannels = std::clamp(fileChannels, 1, 2);
    if (sourceFrame < 0) {
        sourceFrame = 0;
    }
    const std::int64_t firstBlock = sourceFrame / static_cast<std::int64_t>(kBlockFrames);
    const std::int64_t lastBlock = (sourceFrame + frameCount - 1) / static_cast<std::int64_t>(kBlockFrames);
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    for (Block& block : blocks_) {
        if (!block.occupied || block.fileId != fileId || block.rate != outputRate) {
            continue;
        }
        block.anchor = std::abs(static_cast<double>(block.index - firstBlock));
        if (block.index >= firstBlock && block.index <= lastBlock) {
            block.touchedAt = now;
        }
    }
    for (std::int64_t blockIndex = firstBlock; blockIndex <= lastBlock; ++blockIndex) {
        if (findBlock(fileId, outputRate, blockIndex) != nullptr) {
            continue;
        }
        const size_t incoming = kBlockFrames * static_cast<size_t>(storedChannels) * sizeof(std::int16_t);
        Block* block = claimBlock(fileId, blockIndex, outputRate, storedChannels, incoming);
        if (block == nullptr) {
            cacheMiss = true;
            return false;
        }
        if (!fillBlock(decoded, zipWav, *block, error)) {
            releaseBlock(*block);
            cacheMiss = true;
            return false;
        }
    }
    const Block* current = nullptr;
    std::int64_t currentIndex = -1;
    for (int frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
        const std::int64_t absolute = sourceFrame + frameIndex;
        const std::int64_t blockIndex = absolute / static_cast<std::int64_t>(kBlockFrames);
        const size_t offset = static_cast<size_t>(absolute % static_cast<std::int64_t>(kBlockFrames));
        if (blockIndex != currentIndex) {
            current = findBlock(fileId, outputRate, blockIndex);
            currentIndex = blockIndex;
        }
        for (int channel = 0; channel < channelCount; ++channel) {
            float sample = 0;
            if (current != nullptr) {
                const int sourceChannel = storedChannels == 1 ? 0 : std::min(channel, storedChannels - 1);
                sample = static_cast<float>(current->pcm[offset * static_cast<size_t>(storedChannels) +
                                                         static_cast<size_t>(sourceChannel)]) /
                         32767.0f;
            }
            interleaved[frameIndex * channelCount + channel] = sample;
        }
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
    const int id = identify(file);
    if (ZipWav* zipWav = zipWavFor(file, error)) {
        return readCached(id, nullptr, zipWav, outputRate, sourceFrame, interleaved, frameCount, channelCount, cacheMiss,
                          error);
    }
    if (Decoded* decoded = decodedFor(file, error)) {
        return readCached(id, decoded, nullptr, outputRate, sourceFrame, interleaved, frameCount, channelCount, cacheMiss,
                          error);
    }
    writeSilence(interleaved, frameCount, channelCount);
    return true;
}

}  // namespace dawplay
