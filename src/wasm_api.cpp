#include "dawplay/engine.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct DawSession {
    std::vector<std::uint8_t> input;
    std::shared_ptr<dawplay::AudioStore> store;
    dawplay::Project project;
    std::unique_ptr<dawplay::Engine> engine;
    std::string error;
    std::string status;
    std::mutex renderMutex;
    std::thread renderThread;
    std::atomic<int> renderState{0};
    std::vector<std::uint8_t> renderBytes;
    std::string renderError;
    int renderRate = 48000;
    dawplay::PcmFormat renderFormat = dawplay::PcmFormat::F32;
    bool renderMulti = false;
};

void appendJson(std::string& out, const std::string& text) {
    out.push_back('"');
    for (char character : text) {
        if (character == '"' || character == '\\') {
            out.push_back('\\');
        }
        out.push_back(character);
    }
    out.push_back('"');
}

}  // namespace

extern "C" {

DawSession* daw_create(void) { return new DawSession(); }

void daw_destroy(DawSession* session) {
    if (session == nullptr) {
        return;
    }
    if (session->engine) {
        session->engine->pause();
    }
    if (session->renderThread.joinable()) {
        session->renderThread.join();
    }
    delete session;
}

std::uint8_t* daw_resize_input(DawSession* session, std::size_t size) {
    if (session == nullptr) {
        return nullptr;
    }
    if (session->renderThread.joinable()) {
        session->renderThread.join();
    }
    session->engine.reset();
    session->store.reset();
    session->input.assign(size, 0);
    session->renderBytes.clear();
    session->renderState.store(0);
    return session->input.data();
}

int daw_load(DawSession* session) {
    if (session == nullptr) {
        return 1;
    }
    dawplay::LoadResult loaded =
        dawplay::loadProjectBytes(session->input.data(), session->input.size(), "upload.dawproject");
    if (!loaded.error.empty()) {
        session->error = loaded.error;
        return 1;
    }
    const dawplay::StretchPreset preset =
        session->engine ? session->engine->preset() : dawplay::StretchPreset::Cheaper;
    session->store = std::move(loaded.store);
    session->project = std::move(loaded.project);
    dawplay::EngineSettings settings;
    settings.preset = preset;
    session->engine = std::make_unique<dawplay::Engine>(session->store, session->project, settings);
    session->error.clear();
    return 0;
}

const char* daw_last_error(DawSession* session) {
    if (session == nullptr) {
        return "No session";
    }
    return session->error.c_str();
}

void daw_play(DawSession* session) {
    if (session != nullptr && session->engine) {
        session->engine->play();
    }
}

void daw_pause(DawSession* session) {
    if (session != nullptr && session->engine) {
        session->engine->pause();
    }
}

void daw_stop(DawSession* session) {
    if (session != nullptr && session->engine) {
        session->engine->stop();
    }
}

void daw_seek_seconds(DawSession* session, double seconds) {
    if (session != nullptr && session->engine) {
        session->engine->seekSeconds(seconds);
    }
}

void daw_seek_bar(DawSession* session, double bar) {
    if (session != nullptr && session->engine) {
        session->engine->seekBar(bar);
    }
}

void daw_set_preset(DawSession* session, int preset) {
    if (session != nullptr && session->engine) {
        session->engine->setStretchPreset(preset == 0 ? dawplay::StretchPreset::Cheaper : dawplay::StretchPreset::Default);
    }
}

void daw_set_hardware(DawSession* session, int channel, int start, int width) {
    if (session != nullptr && session->engine) {
        session->engine->setHardwareOutput(channel, start, width);
    }
}

void daw_set_cache_mib(DawSession* session, int mib) {
    if (session != nullptr && session->engine) {
        const int used = mib < 1 ? 1 : mib;
        session->engine->setCacheBudget(static_cast<std::size_t>(used) * 1024 * 1024);
    }
}

int daw_start_device(DawSession* session) {
    if (session == nullptr || !session->engine) {
        return 1;
    }
    std::string error;
    if (!session->engine->startDevice(error)) {
        session->error = error;
        return 1;
    }
    session->error.clear();
    return 0;
}

int daw_render_start(DawSession* session, int rate, const char* formatName, int multichannel) {
    if (session == nullptr || !session->engine || session->renderState.load() == 1) {
        return 1;
    }
    if (session->renderThread.joinable()) {
        session->renderThread.join();
    }
    dawplay::PcmFormat format = dawplay::PcmFormat::F32;
    if (formatName != nullptr && !dawplay::pcmFormatFromName(formatName, format)) {
        session->error = "Unknown PCM format";
        return 1;
    }
    session->renderRate = rate < 8000 ? 8000 : rate;
    session->renderFormat = format;
    session->renderMulti = multichannel != 0;
    session->renderBytes.clear();
    session->renderError.clear();
    session->renderState.store(1);
    const int sampleRate = session->renderRate;
    const dawplay::PcmFormat pcm = session->renderFormat;
    const bool multi = session->renderMulti;
    const dawplay::StretchPreset preset = session->engine->preset();
    const dawplay::Project project = session->engine->project();
    auto store = session->store;
    session->renderThread = std::thread([session, sampleRate, pcm, multi, preset, project, store] {
        dawplay::EngineSettings settings;
        settings.outputRate = sampleRate;
        settings.preset = preset;
        dawplay::Engine renderer(store, project, settings);
        dawplay::RenderRequest request;
        request.path = "dawplay-render.wav";
        request.sampleRate = sampleRate;
        request.format = pcm;
        request.multichannel = multi;
        std::string error;
        if (!renderer.renderToWav(request, error)) {
            std::lock_guard<std::mutex> lock(session->renderMutex);
            session->renderError = error;
            session->renderState.store(3);
            return;
        }
        std::ifstream input(request.path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        std::remove(request.path.c_str());
        std::lock_guard<std::mutex> lock(session->renderMutex);
        session->renderBytes = std::move(bytes);
        session->renderState.store(2);
    });
    return 0;
}

int daw_render_state(DawSession* session) {
    if (session == nullptr) {
        return 0;
    }
    return session->renderState.load();
}

const std::uint8_t* daw_render_data(DawSession* session) {
    if (session == nullptr) {
        return nullptr;
    }
    return session->renderBytes.data();
}

int daw_render_size(DawSession* session) {
    if (session == nullptr) {
        return 0;
    }
    return static_cast<int>(session->renderBytes.size());
}

const char* daw_status_json(DawSession* session) {
    if (session == nullptr || !session->engine) {
        return "{\"loaded\":false}";
    }
    const dawplay::Engine& engine = *session->engine;
    const dawplay::MemoryStats stats = engine.memory();
    std::string& out = session->status;
    out.clear();
    out += "{\"loaded\":true,\"playing\":";
    out += engine.playing() ? "true" : "false";
    out += ",\"seconds\":" + std::to_string(engine.positionSeconds());
    out += ",\"beats\":" + std::to_string(engine.positionBeats());
    out += ",\"bar\":" + std::to_string(engine.positionBar());
    out += ",\"tempo\":" + std::to_string(engine.project().tempo.bpmAt(engine.positionBeats()));
    out += ",\"cpu\":" + std::to_string(engine.cpuPercent());
    out += ",\"callback\":" + std::to_string(engine.callbackLoad());
    out += ",\"underruns\":" + std::to_string(engine.underruns());
    out += ",\"cache\":" + std::to_string(stats.cacheBytes);
    out += ",\"budget\":" + std::to_string(stats.cacheBudget);
    out += ",\"stretch\":" + std::to_string(stats.stretcherBytes);
    out += ",\"stretchers\":" + std::to_string(stats.activeStretchers);
    out += ",\"fifo\":" + std::to_string(stats.fifoBytes);
    out += ",\"model\":" + std::to_string(stats.modelBytes);
    out += ",\"resident\":" + std::to_string(stats.residentBytes);
    out += ",\"preset\":";
    appendJson(out, dawplay::stretchPresetName(engine.preset()));
    out += ",\"error\":";
    appendJson(out, session->error);
    out += ",\"renderState\":" + std::to_string(session->renderState.load());
    out += ",\"warnings\":[";
    for (size_t warningIndex = 0; warningIndex < engine.project().warnings.size(); ++warningIndex) {
        if (warningIndex > 0) {
            out += ',';
        }
        appendJson(out, engine.project().warnings[warningIndex]);
    }
    out += "],\"tracks\":[";
    for (size_t channelIndex = 0; channelIndex < engine.project().channels.size(); ++channelIndex) {
        const dawplay::MixerChannel& channel = engine.project().channels[channelIndex];
        if (channelIndex > 0) {
            out += ',';
        }
        out += "{\"name\":";
        appendJson(out, channel.name);
        out += ",\"role\":";
        appendJson(out, channel.role);
        out += ",\"solo\":";
        out += channel.solo ? "true" : "false";
        out += ",\"hardware\":" + std::to_string(channel.hardwareStart);
        out += ",\"width\":" + std::to_string(channel.hardwareWidth);
        out += '}';
    }
    out += "]}";
    return out.c_str();
}

}  // extern "C"
