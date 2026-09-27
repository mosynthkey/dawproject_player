#include "dawplay/engine.hpp"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "miniz.h"
#pragma GCC diagnostic pop

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", "tests/test_player.cpp", line, expression);
        ++gFailures;
    }
}

#define CHECK(condition) check(static_cast<bool>(condition), #condition, __LINE__)

std::vector<std::uint8_t> makeSineWav(int sampleRate, double seconds, double frequency, double amplitude) {
    const int frames = static_cast<int>(std::llround(seconds * sampleRate));
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(frames * 2);
    std::vector<std::uint8_t> wav(44 + dataBytes);
    auto write32 = [&](size_t offset, std::uint32_t value) {
        wav[offset] = static_cast<std::uint8_t>(value & 0xFF);
        wav[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        wav[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
        wav[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    };
    auto write16 = [&](size_t offset, std::uint16_t value) {
        wav[offset] = static_cast<std::uint8_t>(value & 0xFF);
        wav[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    };
    std::memcpy(wav.data(), "RIFF", 4);
    write32(4, static_cast<std::uint32_t>(wav.size() - 8));
    std::memcpy(wav.data() + 8, "WAVE", 4);
    std::memcpy(wav.data() + 12, "fmt ", 4);
    write32(16, 16);
    write16(20, 1);
    write16(22, 1);
    write32(24, static_cast<std::uint32_t>(sampleRate));
    write32(28, static_cast<std::uint32_t>(sampleRate * 2));
    write16(32, 2);
    write16(34, 16);
    std::memcpy(wav.data() + 36, "data", 4);
    write32(40, dataBytes);
    for (int frame = 0; frame < frames; ++frame) {
        const double time = static_cast<double>(frame) / static_cast<double>(sampleRate);
        const int sample = static_cast<int>(std::lrint(std::sin(2.0 * 3.14159265358979323846 * frequency * time) *
                                                        amplitude * 32767.0));
        const auto quantized = static_cast<std::int16_t>(std::max(-32767, std::min(32767, sample)));
        std::memcpy(wav.data() + 44 + static_cast<size_t>(frame) * 2, &quantized, 2);
    }
    return wav;
}

struct ZipEntry {
    std::string name;
    std::vector<std::uint8_t> bytes;
    int level = 0;
};

bool writeZip(const std::filesystem::path& path, const std::string& xml, const std::vector<ZipEntry>& entries) {
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!mz_zip_writer_init_file(&zip, path.string().c_str(), 0)) {
        return false;
    }
    if (!mz_zip_writer_add_mem(&zip, "project.xml", xml.data(), xml.size(), 0)) {
        mz_zip_writer_end(&zip);
        return false;
    }
    for (const ZipEntry& entry : entries) {
        if (!mz_zip_writer_add_mem(&zip, entry.name.c_str(), entry.bytes.data(), entry.bytes.size(),
                                   static_cast<mz_uint>(entry.level))) {
            mz_zip_writer_end(&zip);
            return false;
        }
    }
    const bool finalized = mz_zip_writer_finalize_archive(&zip) && mz_zip_writer_end(&zip);
    return finalized;
}

std::string projectXml(const std::string& fileName, bool external, const std::string& algorithm, double beats,
                       double contentSeconds, double fadeSeconds, bool muted) {
    return std::string("<Project version=\"1.0\">") + "<Application name=\"Test\"/>" +
           "<Transport><Tempo value=\"120\" unit=\"bpm\" id=\"tempo\"/>" +
           "<TimeSignature numerator=\"4\" denominator=\"4\" id=\"sig\"/></Transport>" +
           "<Structure><Track id=\"track1\" name=\"Tone\" loaded=\"true\">" +
           "<Channel id=\"master\" role=\"master\" audioChannels=\"2\">" +
           "<Volume unit=\"linear\" value=\"1\" id=\"vol\"/>" +
           "<Pan unit=\"normalized\" value=\"0.5\" id=\"pan\"/>" + "<Mute value=\"" + (muted ? "true" : "false") +
           "\" id=\"mute\"/></Channel></Track></Structure>" +
           "<Arrangement><Lanes timeUnit=\"beats\"><Lanes track=\"track1\" timeUnit=\"beats\"><Clips>" +
           "<Clip time=\"0\" duration=\"" + std::to_string(beats) + "\" name=\"sine\" fadeInTime=\"" +
           std::to_string(fadeSeconds) + "\" fadeTimeUnit=\"seconds\" enable=\"true\">" +
           "<Warps timeUnit=\"beats\" contentTimeUnit=\"seconds\">" + "<Audio algorithm=\"" + algorithm +
           "\" channels=\"1\" duration=\"1\" sampleRate=\"48000\"><File path=\"" + fileName + "\" external=\"" +
           (external ? "true" : "false") + "\"/></Audio>" + "<Warp time=\"0\" contentTime=\"0\"/>" + "<Warp time=\"" +
           std::to_string(beats) + "\" contentTime=\"" + std::to_string(contentSeconds) +
           "\"/></Warps></Clip></Clips></Lanes></Lanes></Arrangement></Project>";
}

double rmsRange(const std::vector<float>& interleaved, int channels, int first, int last) {
    double energy = 0;
    int count = 0;
    for (int frame = first; frame < last; ++frame) {
        const float sample = interleaved[static_cast<size_t>(frame) * static_cast<size_t>(channels)];
        energy += static_cast<double>(sample) * static_cast<double>(sample);
        ++count;
    }
    return count == 0 ? 0 : std::sqrt(energy / static_cast<double>(count));
}

float peakRange(const std::vector<float>& interleaved, int channels, int first, int last) {
    float peak = 0;
    for (int frame = first; frame < last; ++frame) {
        peak = std::max(peak, std::fabs(interleaved[static_cast<size_t>(frame) * static_cast<size_t>(channels)]));
    }
    return peak;
}

std::vector<float> readF32Wav(const std::filesystem::path& path, int& channels, int& rate) {
    FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        return {};
    }
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    std::vector<std::uint8_t> bytes(static_cast<size_t>(length));
    if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
        std::fclose(file);
        return {};
    }
    std::fclose(file);
    channels = 0;
    rate = 0;
    int bits = 0;
    const std::uint8_t* pcm = nullptr;
    size_t pcmBytes = 0;
    size_t cursor = 12;
    while (cursor + 8 <= bytes.size()) {
        const std::uint32_t chunkSize = bytes[cursor + 4] | (bytes[cursor + 5] << 8) | (bytes[cursor + 6] << 16) |
                                        (bytes[cursor + 7] << 24);
        if (std::memcmp(bytes.data() + cursor, "fmt ", 4) == 0 && chunkSize >= 16) {
            channels = bytes[cursor + 8 + 2] | (bytes[cursor + 8 + 3] << 8);
            rate = static_cast<int>(bytes[cursor + 8 + 4] | (bytes[cursor + 8 + 5] << 8) | (bytes[cursor + 8 + 6] << 16) |
                                    (bytes[cursor + 8 + 7] << 24));
            bits = bytes[cursor + 8 + 14] | (bytes[cursor + 8 + 15] << 8);
        } else if (std::memcmp(bytes.data() + cursor, "data", 4) == 0) {
            pcm = bytes.data() + cursor + 8;
            pcmBytes = chunkSize;
        }
        cursor += 8 + chunkSize + (chunkSize & 1u);
    }
    if (pcm == nullptr || bits != 32 || channels <= 0) {
        return {};
    }
    const size_t samples = pcmBytes / sizeof(float);
    std::vector<float> audio(samples);
    std::memcpy(audio.data(), pcm, samples * sizeof(float));
    return audio;
}

void testTempo() {
    dawplay::TempoMap tempo;
    tempo.points.push_back(dawplay::TempoPoint{0, 120, false});
    CHECK(std::abs(tempo.secondsAtBeat(4) - 2.0) < 1.0e-6);
    CHECK(std::abs(tempo.beatsAtSeconds(2.0) - 4.0) < 1.0e-4);

    dawplay::TempoMap ramp;
    ramp.points.push_back(dawplay::TempoPoint{0, 120, true});
    ramp.points.push_back(dawplay::TempoPoint{4, 60, false});
    const double expected = 4.0 * std::log(2.0);
    CHECK(std::abs(ramp.secondsBetweenBeats(0, 4) - expected) < 1.0e-4);

    dawplay::TimeSigMap signature;
    signature.points.push_back(dawplay::TimeSigPoint{0, 4, 4});
    CHECK(std::abs(signature.beatsAtBar(1) - 0.0) < 1.0e-9);
    CHECK(std::abs(signature.beatsAtBar(5) - 16.0) < 1.0e-9);
    CHECK(std::abs(signature.barAtBeats(0) - 1.0) < 1.0e-9);
}

void testRender(const std::filesystem::path& directory, const std::vector<std::uint8_t>& sine) {
    const std::string xml = projectXml("tone.wav", false, "hd-stretch", 2.0, 1.0, 0.1, false);
    const auto projectPath = directory / "ratio.dawproject";
    CHECK(writeZip(projectPath, xml, {ZipEntry{"tone.wav", sine, 0}, ZipEntry{"tone.mp3", sine, 0}}));
    dawplay::LoadResult loaded = dawplay::loadProjectFile(projectPath.string());
    CHECK(loaded.error.empty());
    CHECK(loaded.project.events.size() == 1);
    CHECK(std::abs(loaded.project.lengthSeconds() - 1.0) < 1.0e-3);
    CHECK(std::abs(loaded.project.events[0].fadeInSecond - 0.1) < 1.0e-3);
    CHECK(!loaded.project.warnings.empty());

    dawplay::EngineSettings settings;
    settings.outputRate = 48000;
    dawplay::Engine engine(loaded.store, loaded.project, settings);
    dawplay::RenderRequest request;
    request.path = (directory / "ratio.wav").string();
    request.sampleRate = 48000;
    request.format = dawplay::PcmFormat::F32;
    std::string error;
    CHECK(engine.renderToWav(request, error));
    int channels = 0;
    int rate = 0;
    const std::vector<float> audio = readF32Wav(directory / "ratio.wav", channels, rate);
    CHECK(rate == 48000);
    CHECK(channels == 2);
    CHECK(audio.size() == static_cast<size_t>(48000 * 2));
    const double startRms = rmsRange(audio, 2, 0, 960);
    const double midRms = rmsRange(audio, 2, 19200, 28800);
    const float midPeak = peakRange(audio, 2, 19200, 28800);
    CHECK(startRms < 0.05);
    CHECK(std::abs(midRms - 0.25) < 0.05);
    CHECK(std::abs(midPeak - 0.353) < 0.05);
    CHECK(engine.memory().activeStretchers == 0);

    request.format = dawplay::PcmFormat::S16;
    request.path = (directory / "ratio-s16.wav").string();
    CHECK(engine.renderToWav(request, error));
    FILE* encoded = std::fopen(request.path.c_str(), "rb");
    char riff[4] = {};
    CHECK(encoded != nullptr && std::fread(riff, 1, 4, encoded) == 4 && std::memcmp(riff, "RIFF", 4) == 0);
    if (encoded != nullptr) {
        std::fclose(encoded);
    }
    for (dawplay::PcmFormat format : {dawplay::PcmFormat::S32, dawplay::PcmFormat::S24, dawplay::PcmFormat::U8}) {
        request.format = format;
        request.path = (directory / (std::string("ratio-") + dawplay::pcmFormatName(format) + ".wav")).string();
        CHECK(engine.renderToWav(request, error));
    }

    const std::string mutedXml = projectXml("tone.wav", false, "stretch", 2.0, 1.0, 0.0, true);
    CHECK(writeZip(directory / "muted.dawproject", mutedXml, {ZipEntry{"tone.wav", sine, 0}}));
    dawplay::LoadResult muted = dawplay::loadProjectFile((directory / "muted.dawproject").string());
    CHECK(muted.error.empty());
    dawplay::Engine mutedEngine(muted.store, muted.project, settings);
    request.format = dawplay::PcmFormat::F32;
    request.path = (directory / "muted.wav").string();
    CHECK(mutedEngine.renderToWav(request, error));
    const std::vector<float> silent = readF32Wav(directory / "muted.wav", channels, rate);
    CHECK(rmsRange(silent, 2, 0, 48000) < 1.0e-5);

    const std::string deflated = projectXml("tone.wav", false, "stretch", 2.0, 1.0, 0.0, false);
    CHECK(writeZip(directory / "deflated.dawproject", deflated, {ZipEntry{"tone.wav", sine, 1}}));
    dawplay::LoadResult inflated = dawplay::loadProjectFile((directory / "deflated.dawproject").string());
    CHECK(inflated.error.empty());
    dawplay::Engine inflatedEngine(inflated.store, inflated.project, settings);
    request.path = (directory / "deflated.wav").string();
    CHECK(inflatedEngine.renderToWav(request, error));
    const std::vector<float> deflatedAudio = readF32Wav(directory / "deflated.wav", channels, rate);
    CHECK(std::abs(rmsRange(deflatedAudio, 2, 19200, 28800) - 0.25) < 0.05);
}

void testStretch(const std::filesystem::path& directory, const std::vector<std::uint8_t>& sine) {
    const std::string xml = projectXml("tone.wav", false, "stretch", 4.0, 1.0, 0.0, false);
    CHECK(writeZip(directory / "stretch.dawproject", xml, {ZipEntry{"tone.wav", sine, 0}}));
    dawplay::LoadResult loaded = dawplay::loadProjectFile((directory / "stretch.dawproject").string());
    CHECK(loaded.error.empty());
    CHECK(std::abs(loaded.project.lengthSeconds() - 2.0) < 1.0e-3);
    dawplay::EngineSettings settings;
    settings.outputRate = 48000;
    dawplay::Engine engine(loaded.store, loaded.project, settings);
    dawplay::RenderRequest request;
    request.path = (directory / "stretch.wav").string();
    request.sampleRate = 48000;
    request.format = dawplay::PcmFormat::F32;
    std::string error;
    CHECK(engine.renderToWav(request, error));
    int channels = 0;
    int rate = 0;
    const std::vector<float> audio = readF32Wav(directory / "stretch.wav", channels, rate);
    CHECK(audio.size() == static_cast<size_t>(96000 * 2));
    CHECK(rmsRange(audio, 2, 48000, 96000) > 0.05);
}

void testExternalAndMissing(const std::filesystem::path& directory, const std::vector<std::uint8_t>& sine) {
    const auto wavPath = directory / "tone.wav";
    FILE* wav = std::fopen(wavPath.string().c_str(), "wb");
    CHECK(wav != nullptr);
    if (wav != nullptr) {
        CHECK(std::fwrite(sine.data(), 1, sine.size(), wav) == sine.size());
        std::fclose(wav);
    }
    const std::string xml = projectXml("tone.wav", true, "stretch", 2.0, 1.0, 0.0, false);
    CHECK(writeZip(directory / "external.dawproject", xml, {}));
    dawplay::LoadResult loaded = dawplay::loadProjectFile((directory / "external.dawproject").string());
    CHECK(loaded.error.empty());
    dawplay::EngineSettings settings;
    dawplay::Engine engine(loaded.store, loaded.project, settings);
    dawplay::RenderRequest request;
    request.path = (directory / "external.wav").string();
    request.sampleRate = 48000;
    request.format = dawplay::PcmFormat::F32;
    std::string error;
    CHECK(engine.renderToWav(request, error));
    int channels = 0;
    int rate = 0;
    const std::vector<float> audio = readF32Wav(directory / "external.wav", channels, rate);
    CHECK(std::abs(rmsRange(audio, 2, 19200, 28800) - 0.25) < 0.05);

    const std::string missingXml = projectXml("missing.wav", false, "stretch", 2.0, 1.0, 0.0, false);
    CHECK(writeZip(directory / "missing.dawproject", missingXml, {}));
    dawplay::LoadResult missing = dawplay::loadProjectFile((directory / "missing.dawproject").string());
    CHECK(missing.error.empty());
    dawplay::Engine missingEngine(missing.store, missing.project, settings);
    request.path = (directory / "missing.wav").string();
    CHECK(missingEngine.renderToWav(request, error));
    const std::vector<float> silence = readF32Wav(directory / "missing.wav", channels, rate);
    CHECK(rmsRange(silence, 2, 0, static_cast<int>(silence.size() / 2)) < 1.0e-5);
}

void testCache(const std::filesystem::path& directory) {
    dawplay::LoadResult loaded = dawplay::loadProjectFile((directory / "ratio.dawproject").string());
    CHECK(loaded.error.empty());
    loaded.store->setBudget(dawplay::AudioStore::kBlockFrames * sizeof(std::int16_t));
    dawplay::FileRef wav = loaded.project.events[0].file;
    std::vector<float> frames(256, 1.0f);
    bool cacheMiss = false;
    CHECK(loaded.store->readFrames(wav, 48000, 0, frames.data(), 256, 1, cacheMiss));
    CHECK(!cacheMiss);
    CHECK(loaded.store->bytesInUse() == 0);

    dawplay::FileRef compressed;
    compressed.path = "tone.mp3";
    compressed.channels = 1;
    compressed.sampleRate = 48000;
    compressed.duration = 1;
    CHECK(loaded.store->readFrames(compressed, 48000, 0, frames.data(), 256, 1, cacheMiss));
    CHECK(!cacheMiss);
    CHECK(loaded.store->bytesInUse() > 0);
    CHECK(loaded.store->bytesInUse() <= loaded.store->budget());
    const size_t used = loaded.store->bytesInUse();
    std::vector<float> wide(static_cast<size_t>(dawplay::AudioStore::kBlockFrames) * 3, 0.0f);
    const bool served = loaded.store->readFrames(compressed, 48000, 0, wide.data(),
                                                  static_cast<int>(dawplay::AudioStore::kBlockFrames * 3), 1, cacheMiss);
    CHECK(!served);
    CHECK(cacheMiss);
    CHECK(loaded.store->bytesInUse() <= loaded.store->budget());
    CHECK(loaded.store->bytesInUse() >= used);
}

void testArrangementEnd(const std::filesystem::path& directory) {
    dawplay::LoadResult loaded = dawplay::loadProjectFile((directory / "ratio.dawproject").string());
    CHECK(loaded.error.empty());
    const double length = loaded.project.lengthSeconds();
    dawplay::EngineSettings settings;
    settings.outputRate = 48000;
    dawplay::Engine engine(loaded.store, loaded.project, settings);
    engine.warmOpening(2.0);
    CHECK(!engine.playing());
    CHECK(engine.positionSeconds() < 1.0e-4);
    CHECK(!engine.atArrangementEnd());

    engine.play();
    std::vector<float> buffer(static_cast<size_t>(256 * 2));
    for (int step = 0; step < 10; ++step) {
        engine.pump(buffer.data(), 256);
    }
    engine.pause();
    CHECK(!engine.atArrangementEnd());
    CHECK(engine.positionSeconds() < length);

    engine.play();
    for (int step = 0; step < 400 && engine.playing(); ++step) {
        engine.pump(buffer.data(), 256);
    }
    CHECK(!engine.playing());
    CHECK(engine.atArrangementEnd());
    CHECK(std::abs(engine.positionSeconds() - length) < 1.0e-3);
    const double endedAt = engine.positionSeconds();
    for (int step = 0; step < 20; ++step) {
        engine.pump(buffer.data(), 256);
    }
    CHECK(!engine.playing());
    CHECK(std::abs(engine.positionSeconds() - endedAt) < 1.0e-6);
}

void testStretcherCap(const std::filesystem::path& directory, const std::vector<std::uint8_t>& sine) {
    std::string clips;
    for (int clipIndex = 0; clipIndex < 20; ++clipIndex) {
        clips += "<Clip time=\"0\" duration=\"0.5\" name=\"clip\">";
        clips += "<Warps timeUnit=\"beats\" contentTimeUnit=\"seconds\">";
        clips += "<Audio algorithm=\"stretch\" channels=\"1\" duration=\"1\" sampleRate=\"48000\"><File path=\"tone.wav\"/></Audio>";
        clips += "<Warp time=\"0\" contentTime=\"0\"/><Warp time=\"0.5\" contentTime=\"0.05\"/></Warps></Clip>";
    }
    const std::string xml = std::string("<Project version=\"1.0\"><Application name=\"Cap\"/>") +
                             "<Transport><Tempo value=\"120\" unit=\"bpm\"/><TimeSignature numerator=\"4\" denominator=\"4\"/></Transport>" +
                             "<Structure><Track id=\"track1\" name=\"Tone\"><Channel id=\"master\" role=\"master\" audioChannels=\"2\">" +
                             "<Volume unit=\"linear\" value=\"1\"/><Pan unit=\"normalized\" value=\"0.5\"/></Channel></Track></Structure>" +
                             "<Arrangement><Lanes timeUnit=\"beats\"><Lanes track=\"track1\"><Clips>" + clips +
                             "</Clips></Lanes></Lanes></Arrangement></Project>";
    CHECK(writeZip(directory / "cap.dawproject", xml, {ZipEntry{"tone.wav", sine, 0}}));
    dawplay::LoadResult loaded = dawplay::loadProjectFile((directory / "cap.dawproject").string());
    CHECK(loaded.error.empty());
    CHECK(loaded.project.events.size() == 20);
    dawplay::EngineSettings settings;
    settings.outputRate = 48000;
    dawplay::Engine engine(loaded.store, loaded.project, settings);
    engine.play();
    int peakStretchers = 0;
    std::vector<float> buffer(static_cast<size_t>(256 * 2));
    for (int step = 0; step < 100; ++step) {
        engine.pump(buffer.data(), 256);
        peakStretchers = std::max(peakStretchers, engine.memory().activeStretchers);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    engine.pause();
    CHECK(peakStretchers >= 1);
    CHECK(peakStretchers <= 16);
}

}  // namespace

int main() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "dawplay-tests";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    testTempo();
    const std::vector<std::uint8_t> sine = makeSineWav(48000, 1.0, 440.0, 0.5);
    testRender(directory, sine);
    testArrangementEnd(directory);
    testExternalAndMissing(directory, sine);
    testCache(directory);
    std::printf("stretch and pool checks\n");
    std::fflush(stdout);
    testStretch(directory, sine);
    testStretcherCap(directory, sine);
    if (gFailures != 0) {
        std::fprintf(stderr, "%d checks failed\n", gFailures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
