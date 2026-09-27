#include "dawplay/engine.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <termios.h>

namespace {

void printUsage() {
    std::fputs(
        "Usage:\n"
        "  dawplay <file.dawproject> [more.dawproject ...]\n"
        "  dawplay info <file.dawproject>\n"
        "  dawplay render <file.dawproject> <out.wav> [--rate 48000] [--format f32] [--layout stereo]\n"
        "\n"
        "Formats: f32 s32 s24 s16 u8. Layout: stereo or multi.\n"
        "Keys: space play/pause, s stop, n next, b previous, left/right bar, g bar, t seconds,\n"
        "      up/down track, o hardware output, p preset, d device,\n"
        "      [ ] cache budget, f format, l layout, r render, q quit.\n",
        stderr);
}

void printInfo(const dawplay::LoadResult& loaded) {
    const dawplay::Project& project = loaded.project;
    std::printf("Application: %s\n", project.application.empty() ? "(none)" : project.application.c_str());
    std::printf("Length: %.3f s\n", project.lengthSeconds());
    std::printf("Tempo points: %zu\n", project.tempo.points.size());
    std::printf("Events: %zu\n", project.events.size());
    std::printf("Channels: %zu\n", project.channels.size());
    for (size_t channelIndex = 0; channelIndex < project.channels.size(); ++channelIndex) {
        const dawplay::MixerChannel& channel = project.channels[channelIndex];
        std::printf("  [%zu] %s role=%s solo=%s hw=%d/%d\n", channelIndex,
                    channel.name.empty() ? "(unnamed)" : channel.name.c_str(),
                    channel.role.empty() ? "regular" : channel.role.c_str(), channel.solo ? "yes" : "no",
                    channel.hardwareStart, channel.hardwareWidth);
    }
    for (const std::string& warning : project.warnings) {
        std::printf("Warning: %s\n", warning.c_str());
    }
}

constexpr double kPreloadLeadSeconds = 5.0;
constexpr double kWarmHeadSeconds = 2.0;

std::string fileNameOf(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

struct SongSlot {
    std::unique_ptr<dawplay::Engine> engine;
    std::shared_ptr<dawplay::AudioStore> store;
    std::string error;
};

struct SongOpen {
    std::string path;
    int outputRate = 48000;
    dawplay::StretchPreset preset = dawplay::StretchPreset::Cheaper;
    std::size_t cacheBudget = dawplay::AudioStore::kDefaultBudget;
    int deviceIndex = -1;
    bool warm = true;
};

SongSlot openSong(const SongOpen& request) {
    SongSlot slot;
    dawplay::LoadResult loaded = dawplay::loadProjectFile(request.path);
    if (!loaded.error.empty() || loaded.store == nullptr) {
        slot.error = loaded.error.empty() ? "Could not open the project" : loaded.error;
        return slot;
    }
    loaded.store->setBudget(request.cacheBudget);
    dawplay::EngineSettings settings;
    settings.outputRate = request.outputRate;
    settings.preset = request.preset;
    slot.store = loaded.store;
    slot.engine = std::make_unique<dawplay::Engine>(slot.store, std::move(loaded.project), settings);
    slot.engine->selectDevice(request.deviceIndex);
    if (request.warm) {
        slot.engine->warmOpening(kWarmHeadSeconds);
    }
    return slot;
}

struct PreloadJob {
    std::mutex mutex;
    std::thread thread;
    int index = -1;
    int phase = 0;
    SongSlot song;

    ~PreloadJob() { finish(); }

    void finish() {
        if (thread.joinable()) {
            thread.join();
        }
    }

    std::string status() {
        std::lock_guard<std::mutex> lock(mutex);
        if (phase == 1) {
            return "next: loading";
        }
        if (phase == 2) {
            return "next: ready";
        }
        if (phase == 3) {
            return song.error.empty() ? "next: failed" : "next: " + song.error;
        }
        return {};
    }

    bool readyFor(int target) {
        std::lock_guard<std::mutex> lock(mutex);
        return phase == 2 && index == target && song.engine != nullptr;
    }

    bool busyFor(int target) {
        std::lock_guard<std::mutex> lock(mutex);
        return index == target && phase != 0;
    }

    void begin(int nextIndex, const SongOpen& request) {
        finish();
        {
            std::lock_guard<std::mutex> lock(mutex);
            index = nextIndex;
            phase = 1;
            song = SongSlot{};
        }
        thread = std::thread([this, request, nextIndex] {
            SongSlot opened = openSong(request);
            std::lock_guard<std::mutex> lock(mutex);
            if (index != nextIndex || phase != 1) {
                return;
            }
            song = std::move(opened);
            phase = song.engine != nullptr ? 2 : 3;
        });
    }

    SongSlot take(int target, const SongOpen& request) {
        finish();
        SongSlot ready;
        bool useReady = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (phase == 2 && index == target && song.engine != nullptr) {
                ready = std::move(song);
                useReady = true;
            }
            phase = 0;
            index = -1;
            song = SongSlot{};
        }
        if (useReady) {
            return ready;
        }
        return openSong(request);
    }
};

struct RawTerminal {
    termios saved{};
    bool active = false;

    void enable() {
        if (!isatty(STDIN_FILENO)) {
            return;
        }
        if (tcgetattr(STDIN_FILENO, &saved) != 0) {
            return;
        }
        termios raw = saved;
        raw.c_lflag &= static_cast<unsigned>(~(ICANON | ECHO));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        active = true;
    }

    ~RawTerminal() {
        if (active) {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        }
    }
};

std::string hardwareLabel(int start, int width) {
    if (start < 0) {
        return "follow";
    }
    if (width <= 1) {
        return "out " + std::to_string(start + 1);
    }
    return "out " + std::to_string(start + 1) + "-" + std::to_string(start + width);
}

void drawScreen(dawplay::Engine& engine, int selected, const std::string& prompt, const std::string& notice,
                dawplay::PcmFormat format, bool multichannel, int songIndex, int songCount, const std::string& songName,
                const std::string& preloadStatus) {
    const dawplay::MemoryStats stats = engine.memory();
    std::fputs("\033[H\033[J", stdout);
    std::printf("DAWPROJECT player\n");
    std::printf("%d/%d  %s\n", songIndex + 1, songCount, songName.c_str());
    if (!preloadStatus.empty()) {
        std::printf("%s\n", preloadStatus.c_str());
    }
    std::printf("%s  %s   bar %.2f   beat %.2f   %.2f s   %.1f bpm\n", engine.playing() ? "PLAY" : "STOP",
                engine.project().application.c_str(), engine.positionBar(), engine.positionBeats(),
                engine.positionSeconds(), engine.project().tempo.bpmAt(engine.positionBeats()));
    std::printf("CPU %.1f%%   callback %.1f%%   underruns %llu\n", engine.cpuPercent(), engine.callbackLoad(),
                static_cast<unsigned long long>(engine.underruns()));
    std::printf("cache %zu / %zu   stretch %zu (%d)   fifo %zu   model %zu   rss %zu\n", stats.cacheBytes,
                stats.cacheBudget, stats.stretcherBytes, stats.activeStretchers, stats.fifoBytes, stats.modelBytes,
                stats.residentBytes);
    std::printf("preset %s   render %s %s   rate %d\n", dawplay::stretchPresetName(engine.preset()),
                dawplay::pcmFormatName(format), multichannel ? "multi" : "stereo", engine.outputRate());
    const auto devices = engine.playbackDevices();
    if (engine.selectedDevice() >= 0 && engine.selectedDevice() < static_cast<int>(devices.size())) {
        std::printf("device %s\n", devices[static_cast<size_t>(engine.selectedDevice())].c_str());
    } else {
        std::printf("device default (%zu found)\n", devices.size());
    }
    std::fputs("\n", stdout);
    for (size_t channelIndex = 0; channelIndex < engine.project().channels.size(); ++channelIndex) {
        const dawplay::MixerChannel& channel = engine.project().channels[channelIndex];
        const bool marked = static_cast<int>(channelIndex) == selected;
        std::printf("%c %-16s %-8s %s%s  %s\n", marked ? '>' : ' ',
                    channel.name.empty() ? "(unnamed)" : channel.name.c_str(),
                    channel.role.empty() ? "track" : channel.role.c_str(), channel.mute.fallback >= 0.5 ? "M" : "-",
                    channel.solo ? "S" : "-",
                    hardwareLabel(channel.hardwareStart, channel.hardwareWidth).c_str());
    }
    if (!engine.project().warnings.empty()) {
        std::fputs("\n", stdout);
        for (const std::string& warning : engine.project().warnings) {
            std::printf("! %s\n", warning.c_str());
        }
    }
    if (!notice.empty()) {
        std::printf("\n%s\n", notice.c_str());
    }
    if (!prompt.empty()) {
        std::printf("\n%s", prompt.c_str());
    } else {
        std::fputs(
            "\nspace play  s stop  n next  b previous  arrows seek/select  g bar  t time  o out  p preset  r render  q quit\n",
            stdout);
    }
    std::fflush(stdout);
}

int runTui(const std::vector<std::string>& paths, dawplay::LoadResult loaded) {
    dawplay::EngineSettings settings;
    settings.preset = dawplay::StretchPreset::Cheaper;
    auto engine = std::make_unique<dawplay::Engine>(loaded.store, std::move(loaded.project), settings);
    std::shared_ptr<dawplay::AudioStore> store = loaded.store;
    std::string deviceError;
    if (!engine->startDevice(deviceError)) {
        deviceError = "Playback device unavailable (" + deviceError + "). Render still works.";
    }

    RawTerminal terminal;
    terminal.enable();

    int songIndex = 0;
    int selected = engine->project().channels.empty() ? -1 : 0;
    dawplay::PcmFormat format = dawplay::PcmFormat::F32;
    bool multichannel = false;
    std::string notice = deviceError;
    std::string typed;
    enum class Prompt { None, Bar, Seconds } prompt = Prompt::None;
    std::atomic<int> renderState{0};
    std::mutex renderMutex;
    std::string renderNote;
    std::thread renderThread;
    PreloadJob preload;

    auto finishRender = [&] {
        if (renderThread.joinable()) {
            renderThread.join();
        }
    };

    auto openRequest = [&](int index, bool warm) {
        SongOpen request;
        request.path = paths[static_cast<size_t>(index)];
        request.outputRate = engine->outputRate();
        request.preset = engine->preset();
        request.cacheBudget = engine->memory().cacheBudget;
        request.deviceIndex = engine->selectedDevice();
        request.warm = warm;
        return request;
    };

    auto draw = [&] {
        std::string promptText;
        if (prompt == Prompt::Bar) {
            promptText = "Bar: " + typed;
        } else if (prompt == Prompt::Seconds) {
            promptText = "Seconds: " + typed;
        }
        const std::string preloadStatus =
            songIndex + 1 < static_cast<int>(paths.size()) ? preload.status() : std::string{};
        drawScreen(*engine, selected, promptText, notice, format, multichannel, songIndex,
                   static_cast<int>(paths.size()), fileNameOf(paths[static_cast<size_t>(songIndex)]), preloadStatus);
    };

    auto considerPreload = [&] {
        const int nextIndex = songIndex + 1;
        if (renderState.load() == 1 || nextIndex >= static_cast<int>(paths.size()) || preload.busyFor(nextIndex)) {
            return;
        }
        const double length = engine->project().lengthSeconds();
        const double position = engine->positionSeconds();
        const bool shortSong = length > 0.0 && length <= kPreloadLeadSeconds;
        const bool nearEnd = length > kPreloadLeadSeconds && length - position <= kPreloadLeadSeconds;
        const bool shortAndGoing = shortSong && (engine->playing() || engine->atArrangementEnd());
        if (!nearEnd && !shortAndGoing) {
            return;
        }
        preload.begin(nextIndex, openRequest(nextIndex, true));
    };

    auto switchSong = [&](int target) {
        if (target < 0 || target >= static_cast<int>(paths.size()) || target == songIndex) {
            return false;
        }
        if (renderState.load() == 1) {
            notice = "Wait for the render to finish";
            return false;
        }
        const SongOpen request = openRequest(target, true);
        if (!preload.readyFor(target)) {
            notice = "Loading " + fileNameOf(paths[static_cast<size_t>(target)]);
            draw();
        }
        SongSlot incoming = preload.take(target, request);
        if (incoming.engine == nullptr) {
            notice = incoming.error.empty() ? "Could not open the project" : incoming.error;
            return false;
        }
        const dawplay::StretchPreset preset = engine->preset();
        const std::size_t budget = engine->memory().cacheBudget;
        const int deviceIndex = engine->selectedDevice();
        incoming.engine->setStretchPreset(preset);
        incoming.engine->setCacheBudget(budget);
        incoming.engine->selectDevice(deviceIndex);
        const bool wasPlaying = engine->playing();
        incoming.engine->play();
        engine->pause();
        engine->stopDevice();
        std::string openError;
        if (!incoming.engine->startDevice(openError)) {
            notice = openError;
            std::string restoreError;
            if (!engine->startDevice(restoreError) && !restoreError.empty()) {
                notice += " " + restoreError;
            } else if (wasPlaying) {
                engine->play();
            }
            return false;
        }
        engine = std::move(incoming.engine);
        store = std::move(incoming.store);
        songIndex = target;
        selected = engine->project().channels.empty() ? -1 : 0;
        prompt = Prompt::None;
        typed.clear();
        notice.clear();
        return true;
    };

    bool running = true;
    while (running) {
        considerPreload();
        if (renderState.load() == 2) {
            std::lock_guard<std::mutex> lock(renderMutex);
            notice = renderNote;
            renderState.store(0);
        } else if (renderState.load() == 3) {
            std::lock_guard<std::mutex> lock(renderMutex);
            notice = renderNote;
            renderState.store(0);
        }
        draw();

        char input = 0;
        const ssize_t got = read(STDIN_FILENO, &input, 1);
        if (got <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            continue;
        }
        if (input == '\033') {
            char sequence[2] = {};
            if (read(STDIN_FILENO, &sequence[0], 1) <= 0 || read(STDIN_FILENO, &sequence[1], 1) <= 0) {
                continue;
            }
            if (sequence[0] == '[') {
                if (sequence[1] == 'D') {
                    engine->seekBar(engine->positionBar() - 1.0);
                } else if (sequence[1] == 'C') {
                    engine->seekBar(engine->positionBar() + 1.0);
                } else if (sequence[1] == 'A' && selected > 0) {
                    --selected;
                } else if (sequence[1] == 'B' && selected + 1 < static_cast<int>(engine->project().channels.size())) {
                    ++selected;
                }
            }
            continue;
        }
        if (prompt != Prompt::None) {
            if (input == 27) {
                prompt = Prompt::None;
                typed.clear();
            } else if (input == '\n' || input == '\r') {
                try {
                    const double value = std::stod(typed);
                    if (prompt == Prompt::Bar) {
                        engine->seekBar(value);
                    } else {
                        engine->seekSeconds(value);
                    }
                } catch (...) {
                    notice = "Could not parse that number";
                }
                prompt = Prompt::None;
                typed.clear();
            } else if (input == 127 || input == '\b') {
                if (!typed.empty()) {
                    typed.pop_back();
                }
            } else if ((input >= '0' && input <= '9') || input == '.' || input == '-') {
                typed.push_back(input);
            }
            continue;
        }
        if (input == 'q') {
            running = false;
        } else if (input == ' ') {
            if (engine->playing()) {
                engine->pause();
            } else if (engine->atArrangementEnd()) {
                if (songIndex + 1 >= static_cast<int>(paths.size())) {
                    if (songIndex == 0) {
                        engine->seekSeconds(0);
                        engine->play();
                    } else {
                        switchSong(0);
                    }
                } else {
                    engine->seekSeconds(0);
                    engine->play();
                }
            } else {
                engine->play();
            }
        } else if (input == 'n') {
            if (songIndex + 1 >= static_cast<int>(paths.size())) {
                notice = "End of playlist";
            } else {
                switchSong(songIndex + 1);
            }
        } else if (input == 'b') {
            if (songIndex == 0) {
                notice = "Start of playlist";
            } else {
                switchSong(songIndex - 1);
            }
        } else if (input == 's') {
            engine->stop();
        } else if (input == 'g') {
            prompt = Prompt::Bar;
            typed.clear();
        } else if (input == 't') {
            prompt = Prompt::Seconds;
            typed.clear();
        } else if (input == 'p') {
            engine->setStretchPreset(engine->preset() == dawplay::StretchPreset::Cheaper
                                         ? dawplay::StretchPreset::Default
                                         : dawplay::StretchPreset::Cheaper);
        } else if (input == 'd') {
            const auto devices = engine->playbackDevices();
            if (!devices.empty()) {
                const int next = (engine->selectedDevice() + 1) % static_cast<int>(devices.size());
                engine->selectDevice(next);
            }
        } else if (input == 'o' && selected >= 0) {
            const dawplay::MixerChannel& channel = engine->project().channels[static_cast<size_t>(selected)];
            int start = channel.hardwareStart;
            if (start < 0) {
                start = 0;
            } else {
                start += 2;
            }
            if (start > 6) {
                start = -1;
            }
            engine->setHardwareOutput(selected, start, 2);
        } else if (input == '[') {
            const size_t mib = std::max<size_t>(engine->memory().cacheBudget / (1024 * 1024), 1);
            engine->setCacheBudget((mib > 1 ? mib - 1 : 1) * 1024 * 1024);
        } else if (input == ']') {
            const size_t mib = engine->memory().cacheBudget / (1024 * 1024);
            engine->setCacheBudget((mib + 1) * 1024 * 1024);
        } else if (input == 'f') {
            const char* name = dawplay::pcmFormatName(format);
            if (std::strcmp(name, "f32") == 0) {
                format = dawplay::PcmFormat::S32;
            } else if (std::strcmp(name, "s32") == 0) {
                format = dawplay::PcmFormat::S24;
            } else if (std::strcmp(name, "s24") == 0) {
                format = dawplay::PcmFormat::S16;
            } else if (std::strcmp(name, "s16") == 0) {
                format = dawplay::PcmFormat::U8;
            } else {
                format = dawplay::PcmFormat::F32;
            }
        } else if (input == 'l') {
            multichannel = !multichannel;
        } else if (input == 'r' && renderState.load() == 0) {
            finishRender();
            renderState.store(1);
            notice = "Rendering render.wav";
            const auto requestFormat = format;
            const bool requestMulti = multichannel;
            const int rate = engine->outputRate();
            const dawplay::StretchPreset preset = engine->preset();
            const dawplay::Project project = engine->project();
            auto renderStore = store;
            renderThread = std::thread([&, requestFormat, requestMulti, rate, preset, project, renderStore] {
                dawplay::EngineSettings renderSettings;
                renderSettings.outputRate = rate;
                renderSettings.preset = preset;
                dawplay::Engine renderer(renderStore, project, renderSettings);
                dawplay::RenderRequest request;
                request.path = "render.wav";
                request.sampleRate = rate;
                request.format = requestFormat;
                request.multichannel = requestMulti;
                std::string error;
                const bool ok = renderer.renderToWav(request, error);
                std::lock_guard<std::mutex> lock(renderMutex);
                renderNote = ok ? "Wrote render.wav" : error;
                renderState.store(ok ? 2 : 3);
            });
        }
    }
    engine->pause();
    finishRender();
    preload.finish();
    std::fputs("\033[H\033[J", stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }
    std::string command = argv[1];
    std::string path;
    std::string output;
    int rate = 48000;
    dawplay::PcmFormat format = dawplay::PcmFormat::F32;
    bool multichannel = false;
    bool info = false;
    bool render = false;
    if (command == "info" || command == "render") {
        if (argc < 3) {
            printUsage();
            return 1;
        }
        info = command == "info";
        render = command == "render";
        path = argv[2];
        int index = 3;
        if (render) {
            if (argc < 4) {
                printUsage();
                return 1;
            }
            output = argv[3];
            index = 4;
        }
        for (; index < argc; ++index) {
            const std::string flag = argv[index];
            if ((flag == "--rate" || flag == "--format" || flag == "--layout") && index + 1 >= argc) {
                printUsage();
                return 1;
            }
            if (flag == "--rate") {
                rate = std::stoi(argv[++index]);
            } else if (flag == "--format") {
                if (!dawplay::pcmFormatFromName(argv[++index], format)) {
                    std::fputs("Unknown PCM format\n", stderr);
                    return 1;
                }
            } else if (flag == "--layout") {
                const std::string layout = argv[++index];
                if (layout == "multi") {
                    multichannel = true;
                } else if (layout == "stereo") {
                    multichannel = false;
                } else {
                    std::fputs("Layout must be stereo or multi\n", stderr);
                    return 1;
                }
            } else {
                printUsage();
                return 1;
            }
        }
    } else if (command == "--help" || command == "-h") {
        printUsage();
        return 0;
    } else {
        std::vector<std::string> paths;
        for (int argumentIndex = 1; argumentIndex < argc; ++argumentIndex) {
            const std::string argument = argv[argumentIndex];
            if (argument == "--help" || argument == "-h") {
                printUsage();
                return 0;
            }
            if (!argument.empty() && argument[0] == '-') {
                printUsage();
                return 1;
            }
            paths.push_back(argument);
        }
        if (paths.empty()) {
            printUsage();
            return 1;
        }
        if (!isatty(STDIN_FILENO)) {
            int status = 0;
            for (const std::string& songPath : paths) {
                dawplay::LoadResult listed = dawplay::loadProjectFile(songPath);
                if (!listed.error.empty()) {
                    std::fprintf(stderr, "%s\n", listed.error.c_str());
                    status = 1;
                    continue;
                }
                std::printf("File: %s\n", songPath.c_str());
                printInfo(listed);
            }
            return status;
        }
        dawplay::LoadResult first = dawplay::loadProjectFile(paths[0]);
        if (!first.error.empty()) {
            std::fprintf(stderr, "%s\n", first.error.c_str());
            return 1;
        }
        return runTui(paths, std::move(first));
    }

    dawplay::LoadResult loaded = dawplay::loadProjectFile(path);
    if (!loaded.error.empty()) {
        std::fprintf(stderr, "%s\n", loaded.error.c_str());
        return 1;
    }
    if (info) {
        printInfo(loaded);
        return 0;
    }
    if (render) {
        dawplay::EngineSettings settings;
        settings.outputRate = rate;
        dawplay::Engine engine(std::move(loaded.store), std::move(loaded.project), settings);
        dawplay::RenderRequest request;
        request.path = output;
        request.sampleRate = rate;
        request.format = format;
        request.multichannel = multichannel;
        std::string error;
        if (!engine.renderToWav(request, error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::printf("Wrote %s\n", output.c_str());
        return 0;
    }
    if (!isatty(STDIN_FILENO)) {
        printInfo(loaded);
        return 0;
    }
    return runTui({path}, std::move(loaded));
}
