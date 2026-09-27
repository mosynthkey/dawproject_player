#include "dawplay/project.hpp"

#include "dawplay/sources.hpp"
#include "dawplay/xml.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>

namespace dawplay {
namespace {

struct NodeIndex {
    std::unordered_map<std::string, const XmlNode*> byId;

    void add(const XmlNode& node) {
        const std::string id = node.attribute("id");
        if (!id.empty()) {
            byId.emplace(id, &node);
        }
        for (const XmlNode& child : node.children) {
            add(child);
        }
    }

    const XmlNode* find(const std::string& id) const {
        const auto found = byId.find(id);
        return found == byId.end() ? nullptr : found->second;
    }
};

TimeUnit timeUnitOf(const XmlNode& node, TimeUnit inherited) {
    const std::string value = node.attribute("timeUnit");
    if (value == "seconds") {
        return TimeUnit::Seconds;
    }
    if (value == "beats") {
        return TimeUnit::Beats;
    }
    return inherited;
}

double gainFromUnit(double value, const std::string& unit) {
    if (unit == "decibel") {
        return linearToDecibelGain(value);
    }
    // `normalized` has no defined fader law in the format, so it is a linear gain.
    return value;
}

void sortCurve(AutomationCurve& curve) {
    std::sort(curve.points.begin(), curve.points.end(), [](const AutomationPoint& left, const AutomationPoint& right) {
        return left.time < right.time;
    });
}

void readRealPoints(const XmlNode& points, AutomationCurve& curve, double (*convert)(double, const std::string&),
                    const std::string& unit) {
    for (const XmlNode& child : points.children) {
        if (child.name != "RealPoint" && child.name != "BoolPoint") {
            continue;
        }
        AutomationPoint point;
        point.time = child.number("time", 0);
        if (child.name == "BoolPoint") {
            point.value = child.flag("value", false) ? 1.0 : 0.0;
            point.linear = false;
        } else {
            point.value = convert(child.number("value", curve.fallback), unit);
            point.linear = child.attribute("interpolation") == "linear";
        }
        curve.points.push_back(point);
    }
    sortCurve(curve);
}

const XmlNode* contentChild(const XmlNode& clip) {
    for (const XmlNode& child : clip.children) {
        if (child.name == "Audio" || child.name == "Warps" || child.name == "Clips" || child.name == "Lanes") {
            return &child;
        }
    }
    return nullptr;
}

struct Leaf {
    FileRef file;
    std::vector<ClipLayer> layers;
    bool hasWarp = false;
    WarpMap warp;
    std::vector<LayerAutomation> automation;
    double windowOrigin = 0;
    double windowDuration = 0;
    TimeUnit windowUnit = TimeUnit::Beats;
    double fadeIn = 0;
    double fadeOut = 0;
    bool equalPowerIn = false;
    std::string name;
};

struct Builder {
    const NodeIndex& index;
    const TempoMap& tempo;
    Project& project;

    const XmlNode* resolve(const XmlNode& clip) const {
        if (const XmlNode* content = contentChild(clip)) {
            return content;
        }
        const std::string reference = clip.attribute("reference");
        if (!reference.empty()) {
            return index.find(reference);
        }
        return nullptr;
    }

    static void collectClipAutomation(const XmlNode& clip, LayerAutomation& automation) {
        for (const XmlNode& child : clip.children) {
            if (child.name != "Points") {
                continue;
            }
            std::string expression;
            for (const XmlNode& pointChild : child.children) {
                if (pointChild.name == "Target") {
                    expression = pointChild.attribute("expression");
                }
            }
            AutomationCurve* curve = nullptr;
            if (expression == "gain") {
                curve = &automation.gain;
            } else if (expression == "pan") {
                curve = &automation.pan;
            } else if (expression == "transpose") {
                curve = &automation.transpose;
            }
            if (curve == nullptr) {
                continue;
            }
            const std::string unit = expression == "gain" ? "linear" : "normalized";
            readRealPoints(child, *curve, gainFromUnit, expression == "transpose" ? "semitones" : unit);
        }
    }

    void emitAudio(const XmlNode& audio, const std::vector<ClipLayer>& layers, const WarpMap* warp,
                   const std::vector<LayerAutomation>& automation, const Leaf& shape) {
        AudioEvent event;
        event.file.path = {};
        if (const XmlNode* file = audio.child("File")) {
            event.file.path = file->attribute("path");
            event.file.external = file->flag("external", false);
        }
        event.file.channels = static_cast<int>(audio.number("channels", 2));
        event.file.sampleRate = static_cast<int>(audio.number("sampleRate", 48000));
        event.file.duration = audio.number("duration", 0);
        event.file.algorithm = audio.attribute("algorithm");
        if (!event.file.algorithm.empty() && event.file.algorithm != "stretch") {
            project.warnings.push_back("Unrecognized warp algorithm '" + event.file.algorithm +
                                        "'; using pitch-preserving stretch");
        }
        event.layers = layers;
        event.hasWarp = warp != nullptr && !warp->empty();
        if (warp != nullptr) {
            event.warp = *warp;
        }
        event.automation = automation;
        event.channelIndex = 0;
        event.name = shape.name;
        event.equalPowerIn = shape.equalPowerIn;
        // Leaf fades are already seconds. A negative fade-in has been folded into the window.
        event.fadeInSecond = std::abs(shape.fadeIn);
        event.fadeOutSecond = std::abs(shape.fadeOut);

        const double windowStart = shape.windowOrigin;
        const double windowEnd = shape.windowOrigin + shape.windowDuration;
        if (shape.windowUnit == TimeUnit::Seconds) {
            event.startSecond = windowStart;
            event.endSecond = windowEnd;
        } else {
            event.startSecond = tempo.secondsAtBeat(windowStart);
            event.endSecond = tempo.secondsAtBeat(windowEnd);
        }
        if (!(event.endSecond > event.startSecond)) {
            return;
        }
        project.events.push_back(std::move(event));
    }

    void walkContent(const XmlNode& content, TimeUnit unit, std::vector<ClipLayer> layers,
                     std::vector<LayerAutomation> automation, Leaf shape, const WarpMap* warp) {
        if (content.name == "Audio") {
            emitAudio(content, layers, warp, automation, shape);
            return;
        }
        if (content.name == "Warps") {
            WarpMap local;
            for (const XmlNode& child : content.children) {
                if (child.name == "Warp") {
                    local.points.push_back(WarpPoint{child.number("time", 0), child.number("contentTime", 0)});
                }
            }
            std::sort(local.points.begin(), local.points.end(), [](const WarpPoint& left, const WarpPoint& right) {
                return left.time < right.time;
            });
            for (const XmlNode& child : content.children) {
                if (child.name == "Audio" || child.name == "Clips" || child.name == "Lanes" || child.name == "Warps") {
                    walkContent(child, timeUnitOf(content, unit), layers, automation, shape, &local);
                }
            }
            return;
        }
        if (content.name == "Clips" || content.name == "Lanes") {
            const TimeUnit childUnit = timeUnitOf(content, unit);
            for (const XmlNode& child : content.children) {
                if (child.name == "Clip") {
                    walkClip(child, childUnit, layers, automation, shape);
                } else if (child.name == "Clips" || child.name == "Lanes") {
                    walkContent(child, childUnit, layers, automation, shape, warp);
                }
            }
        }
    }

    void walkClip(const XmlNode& clip, TimeUnit parentUnit, std::vector<ClipLayer> layers,
                  std::vector<LayerAutomation> automation, Leaf shape) {
        if (!clip.flag("enable", true)) {
            return;
        }
        const XmlNode* content = resolve(clip);
        if (content == nullptr) {
            return;
        }
        TimeUnit contentUnit = parentUnit;
        if (clip.attribute("contentTimeUnit") == "seconds") {
            contentUnit = TimeUnit::Seconds;
        } else if (clip.attribute("contentTimeUnit") == "beats") {
            contentUnit = TimeUnit::Beats;
        } else {
            contentUnit = timeUnitOf(*content, parentUnit);
        }

        const double clipTime = clip.number("time", 0);
        double duration = clip.number("duration", 0);
        const double playStart = clip.number("playStart", 0);
        const double playStop = clip.number("playStop", 0);
        if (duration <= 0 && playStop > playStart) {
            duration = playStop - playStart;
        }
        if (duration <= 0 && content->name == "Audio") {
            duration = content->number("duration", 0);
        }
        if (duration <= 0) {
            return;
        }

        TimeUnit fadeUnit = parentUnit;
        if (clip.attribute("fadeTimeUnit") == "seconds") {
            fadeUnit = TimeUnit::Seconds;
        } else if (clip.attribute("fadeTimeUnit") == "beats") {
            fadeUnit = TimeUnit::Beats;
        }
        const double fadeIn = clip.number("fadeInTime", 0);
        const double fadeOut = clip.number("fadeOutTime", 0);
        double windowOrigin = clipTime;
        double windowDuration = duration;
        // A negative fade-in starts the clip earlier without moving the content anchor.
        if (fadeIn < 0 && fadeUnit == parentUnit) {
            windowOrigin += fadeIn;
            windowDuration -= fadeIn;
            shape.equalPowerIn = true;
        }
        if (layers.empty()) {
            shape.windowOrigin = windowOrigin;
            shape.windowDuration = windowDuration;
            shape.windowUnit = parentUnit;
            const double fadeInBeats = std::abs(fadeIn);
            const double fadeOutBeats = std::abs(fadeOut);
            if (fadeUnit == TimeUnit::Seconds) {
                shape.fadeIn = fadeInBeats;
                shape.fadeOut = fadeOutBeats;
            } else if (parentUnit == TimeUnit::Beats) {
                const double clipEnd = clipTime + duration;
                shape.fadeIn = std::abs(tempo.secondsBetweenBeats(clipTime, clipTime + fadeIn));
                shape.fadeOut = std::abs(tempo.secondsBetweenBeats(clipEnd - fadeOut, clipEnd));
            } else {
                const double bpm = std::max(tempo.bpmAt(tempo.beatsAtSeconds(clipTime)), 1.0);
                shape.fadeIn = fadeInBeats * 60.0 / bpm;
                shape.fadeOut = fadeOutBeats * 60.0 / bpm;
            }
            if (!clip.attribute("name").empty()) {
                shape.name = clip.attribute("name");
            }
        }

        ClipLayer layer;
        layer.parentUnit = parentUnit;
        layer.contentUnit = contentUnit;
        layer.origin = windowOrigin;
        layer.duration = windowDuration;
        layer.scheduleOrigin = clipTime;
        layer.playStart = playStart;
        layer.playStop = playStop;
        layer.loopStart = clip.number("loopStart", 0);
        layer.loopEnd = clip.number("loopEnd", 0);
        if (!(layer.loopEnd > layer.loopStart)) {
            layer.loopStart = 0;
            layer.loopEnd = 0;
        }
        layers.push_back(layer);

        LayerAutomation layerAuto;
        layerAuto.layer = static_cast<int>(layers.size() - 1);
        layerAuto.gain.fallback = 1;
        layerAuto.pan.fallback = 0.5;
        layerAuto.transpose.fallback = 0;
        collectClipAutomation(clip, layerAuto);
        if (!layerAuto.gain.points.empty() || !layerAuto.pan.points.empty() || !layerAuto.transpose.points.empty()) {
            automation.push_back(layerAuto);
        }
        walkContent(*content, contentUnit, layers, automation, shape, nullptr);
    }
};

struct ParameterBinding {
    int channelIndex = 0;
    enum class Kind { Volume, Pan, Mute } kind = Kind::Volume;
    std::string unit;
};

void readTransport(const XmlNode& transport, Project& project) {
    if (const XmlNode* tempo = transport.child("Tempo")) {
        TempoPoint point;
        point.beats = 0;
        point.bpm = tempo->number("value", 120);
        point.linear = false;
        project.tempo.points.push_back(point);
        for (const XmlNode& child : transport.children) {
            if (child.name != "Points") {
                continue;
            }
            bool targetsTempo = false;
            for (const XmlNode& pointNode : child.children) {
                if (pointNode.name == "Target" && pointNode.attribute("parameter") == tempo->attribute("id") &&
                    !tempo->attribute("id").empty()) {
                    targetsTempo = true;
                }
            }
            if (!targetsTempo) {
                continue;
            }
            project.tempo.points.clear();
            for (const XmlNode& pointNode : child.children) {
                if (pointNode.name != "RealPoint") {
                    continue;
                }
                TempoPoint automated;
                automated.beats = pointNode.number("time", 0);
                automated.bpm = pointNode.number("value", point.bpm);
                automated.linear = pointNode.attribute("interpolation") == "linear";
                project.tempo.points.push_back(automated);
            }
        }
    }
    if (project.tempo.points.empty()) {
        project.tempo.points.push_back(TempoPoint{});
    }
    std::sort(project.tempo.points.begin(), project.tempo.points.end(), [](const TempoPoint& left, const TempoPoint& right) {
        return left.beats < right.beats;
    });

    if (const XmlNode* signature = transport.child("TimeSignature")) {
        TimeSigPoint point;
        point.numerator = static_cast<int>(signature->number("numerator", 4));
        point.denominator = static_cast<int>(signature->number("denominator", 4));
        project.timeSignature.points.push_back(point);
        for (const XmlNode& child : transport.children) {
            if (child.name != "Points") {
                continue;
            }
            bool targets = false;
            for (const XmlNode& pointNode : child.children) {
                if (pointNode.name == "Target" && pointNode.attribute("parameter") == signature->attribute("id") &&
                    !signature->attribute("id").empty()) {
                    targets = true;
                }
            }
            if (!targets) {
                continue;
            }
            project.timeSignature.points.clear();
            for (const XmlNode& pointNode : child.children) {
                if (pointNode.name != "TimeSignaturePoint") {
                    continue;
                }
                TimeSigPoint automated;
                automated.beats = pointNode.number("time", 0);
                automated.numerator = static_cast<int>(pointNode.number("numerator", 4));
                automated.denominator = static_cast<int>(pointNode.number("denominator", 4));
                project.timeSignature.points.push_back(automated);
            }
        }
    }
    if (project.timeSignature.points.empty()) {
        project.timeSignature.points.push_back(TimeSigPoint{});
    }
}

void readTracks(const XmlNode& track, Project& project, std::unordered_map<std::string, int>& trackChannel,
                std::unordered_map<std::string, ParameterBinding>& parameters) {
    if (!track.flag("loaded", true)) {
        for (const XmlNode& child : track.children) {
            if (child.name == "Track") {
                readTracks(child, project, trackChannel, parameters);
            }
        }
        return;
    }
    const XmlNode* channelNode = track.child("Channel");
    if (channelNode != nullptr && channelNode->attribute("role") != "vca") {
        MixerChannel channel;
        channel.id = channelNode->attribute("id");
        channel.name = track.attribute("name");
        if (channel.name.empty()) {
            channel.name = channelNode->attribute("name");
        }
        channel.role = channelNode->attribute("role");
        channel.destinationId = channelNode->attribute("destination");
        channel.audioChannels = static_cast<int>(channelNode->number("audioChannels", 2));
        channel.solo = channelNode->flag("solo", false);
        channel.volume.fallback = 1;
        channel.pan.fallback = 0.5;
        channel.mute.fallback = 0;
        if (const XmlNode* volume = channelNode->child("Volume")) {
            const std::string unit = volume->attribute("unit");
            const double raw = volume->number("value", unit == "decibel" ? 0.0 : 1.0);
            channel.volume.fallback = gainFromUnit(raw, unit);
            if (!volume->attribute("id").empty()) {
                parameters[volume->attribute("id")] = ParameterBinding{static_cast<int>(project.channels.size()),
                                                                       ParameterBinding::Kind::Volume, unit};
            }
        }
        if (const XmlNode* pan = channelNode->child("Pan")) {
            channel.pan.fallback = pan->number("value", 0.5);
            if (!pan->attribute("id").empty()) {
                parameters[pan->attribute("id")] = ParameterBinding{static_cast<int>(project.channels.size()),
                                                                    ParameterBinding::Kind::Pan, pan->attribute("unit")};
            }
        }
        if (const XmlNode* mute = channelNode->child("Mute")) {
            channel.mute.fallback = mute->flag("value", false) ? 1 : 0;
            if (!mute->attribute("id").empty()) {
                parameters[mute->attribute("id")] = ParameterBinding{static_cast<int>(project.channels.size()),
                                                                     ParameterBinding::Kind::Mute, {}};
            }
        }
        if (!track.attribute("id").empty()) {
            trackChannel[track.attribute("id")] = static_cast<int>(project.channels.size());
        }
        project.channels.push_back(std::move(channel));
    }
    for (const XmlNode& child : track.children) {
        if (child.name == "Track") {
            readTracks(child, project, trackChannel, parameters);
        }
    }
}

void readLanePoints(const XmlNode& node, TimeUnit unit, Project& project,
                    const std::unordered_map<std::string, ParameterBinding>& parameters) {
    for (const XmlNode& child : node.children) {
        if (child.name == "Points") {
            std::string parameterId;
            for (const XmlNode& pointChild : child.children) {
                if (pointChild.name == "Target") {
                    parameterId = pointChild.attribute("parameter");
                }
            }
            const auto found = parameters.find(parameterId);
            if (found == parameters.end()) {
                continue;
            }
            MixerChannel& channel = project.channels[static_cast<size_t>(found->second.channelIndex)];
            AutomationCurve* curve = &channel.volume;
            auto convert = gainFromUnit;
            std::string unitName = found->second.unit;
            if (found->second.kind == ParameterBinding::Kind::Pan) {
                curve = &channel.pan;
                convert = [](double value, const std::string&) { return value; };
            } else if (found->second.kind == ParameterBinding::Kind::Mute) {
                curve = &channel.mute;
                convert = [](double value, const std::string&) { return value; };
            } else if (unitName != "decibel") {
                convert = [](double value, const std::string&) { return value; };
            }
            curve->timeUnit = unit;
            readRealPoints(child, *curve, convert, unitName);
        }
        if (child.name == "Lanes" || child.name == "Clips") {
            readLanePoints(child, timeUnitOf(child, unit), project, parameters);
        }
    }
}

void readArrangement(const XmlNode& arrangement, Project& project, const NodeIndex& index,
                     const std::unordered_map<std::string, int>& trackChannel,
                     const std::unordered_map<std::string, ParameterBinding>& parameters) {
    Builder builder{index, project.tempo, project};
    const std::function<void(const XmlNode&, TimeUnit, int)> walk = [&](const XmlNode& node, TimeUnit unit, int channelIndex) {
        const TimeUnit here = timeUnitOf(node, unit);
        int laneChannel = channelIndex;
        if (node.has("track")) {
            const auto found = trackChannel.find(node.attribute("track"));
            if (found != trackChannel.end()) {
                laneChannel = found->second;
            }
        }
        for (const XmlNode& child : node.children) {
            if (child.name == "Lanes" || child.name == "Clips") {
                walk(child, here, laneChannel);
            } else if (child.name == "Clip" && laneChannel >= 0) {
                const size_t before = project.events.size();
                builder.walkClip(child, here, {}, {}, Leaf{});
                for (size_t eventIndex = before; eventIndex < project.events.size(); ++eventIndex) {
                    project.events[eventIndex].channelIndex = laneChannel;
                }
            }
        }
    };
    if (const XmlNode* lanes = arrangement.child("Lanes")) {
        walk(*lanes, TimeUnit::Beats, -1);
        readLanePoints(*lanes, TimeUnit::Beats, project, parameters);
    } else {
        walk(arrangement, TimeUnit::Beats, -1);
    }
}

void linkDestinations(Project& project) {
    std::unordered_map<std::string, int> byId;
    for (int channelIndex = 0; channelIndex < static_cast<int>(project.channels.size()); ++channelIndex) {
        if (!project.channels[static_cast<size_t>(channelIndex)].id.empty()) {
            byId[project.channels[static_cast<size_t>(channelIndex)].id] = channelIndex;
        }
        if (project.channels[static_cast<size_t>(channelIndex)].role == "master") {
            project.masterIndex = channelIndex;
        }
    }
    if (project.masterIndex < 0 && !project.channels.empty()) {
        project.masterIndex = static_cast<int>(project.channels.size()) - 1;
    }
    for (MixerChannel& channel : project.channels) {
        if (channel.destinationId.empty()) {
            channel.destination = -1;
            continue;
        }
        const auto found = byId.find(channel.destinationId);
        channel.destination = found == byId.end() ? project.masterIndex : found->second;
    }
    if (project.masterIndex >= 0) {
        MixerChannel& master = project.channels[static_cast<size_t>(project.masterIndex)];
        master.hardwareStart = 0;
        master.hardwareWidth = 2;
        master.destination = -1;
    }
}

void markEqualPowerOuts(Project& project) {
    for (AudioEvent& incoming : project.events) {
        if (!incoming.equalPowerIn) {
            continue;
        }
        for (AudioEvent& other : project.events) {
            if (&other == &incoming || !(other.fadeOutSecond > 0)) {
                continue;
            }
            const double fadeOutStart = other.endSecond - other.fadeOutSecond;
            if (other.endSecond > incoming.startSecond && fadeOutStart < incoming.startSecond + incoming.fadeInSecond) {
                other.equalPowerOut = true;
            }
        }
    }
}

size_t countModelBytes(const Project& project) {
    size_t bytes = sizeof(Project);
    bytes += project.application.capacity();
    bytes += project.tempo.points.capacity() * sizeof(TempoPoint);
    bytes += project.timeSignature.points.capacity() * sizeof(TimeSigPoint);
    for (const MixerChannel& channel : project.channels) {
        bytes += sizeof(MixerChannel) + channel.id.capacity() + channel.name.capacity();
        bytes += channel.volume.points.capacity() * sizeof(AutomationPoint);
        bytes += channel.pan.points.capacity() * sizeof(AutomationPoint);
        bytes += channel.mute.points.capacity() * sizeof(AutomationPoint);
    }
    for (const AudioEvent& event : project.events) {
        bytes += sizeof(AudioEvent) + event.file.path.capacity() + event.name.capacity();
        bytes += event.layers.capacity() * sizeof(ClipLayer);
        bytes += event.warp.points.capacity() * sizeof(WarpPoint);
        for (const LayerAutomation& automation : event.automation) {
            bytes += automation.gain.points.capacity() * sizeof(AutomationPoint);
            bytes += automation.pan.points.capacity() * sizeof(AutomationPoint);
            bytes += automation.transpose.points.capacity() * sizeof(AutomationPoint);
        }
    }
    for (const std::string& warning : project.warnings) {
        bytes += warning.capacity();
    }
    return bytes;
}

double convertInto(const TempoMap& tempo, double value, TimeUnit from, TimeUnit to, double anchorFrom) {
    if (from == to) {
        return value;
    }
    if (from == TimeUnit::Beats && to == TimeUnit::Seconds) {
        return tempo.secondsAtBeat(anchorFrom) + tempo.secondsBetweenBeats(anchorFrom, value);
    }
    const double anchorSeconds = anchorFrom;
    const double anchorBeats = tempo.beatsAtSeconds(anchorSeconds);
    return anchorBeats + (tempo.beatsAtSeconds(value) - anchorBeats);
}

double wrappedContent(const ClipLayer& layer, double content) {
    if (!(layer.loopEnd > layer.loopStart) || content < layer.loopEnd) {
        return content;
    }
    const double loopLength = layer.loopEnd - layer.loopStart;
    const double into = std::fmod(content - layer.loopEnd, loopLength);
    const double positive = into < 0 ? into + loopLength : into;
    return layer.loopStart + positive;
}

}  // namespace

bool AudioEvent::sourceAt(const TempoMap& tempo, double arrangementSecond, double& sourceSecond) const {
    double cursor = arrangementSecond;
    TimeUnit cursorUnit = TimeUnit::Seconds;
    for (const ClipLayer& layer : layers) {
        double parentTime = cursor;
        if (layer.parentUnit != cursorUnit) {
            parentTime = convertInto(tempo, cursor, cursorUnit, layer.parentUnit, cursor);
        }
        // The end point belongs to the clip. Callers sample it when a block lands on the boundary.
        if (parentTime < layer.origin || parentTime > layer.origin + layer.duration) {
            return false;
        }
        double local = parentTime - layer.scheduleOrigin;
        if (layer.parentUnit != layer.contentUnit) {
            if (layer.parentUnit == TimeUnit::Beats && layer.contentUnit == TimeUnit::Seconds) {
                local = tempo.secondsBetweenBeats(layer.scheduleOrigin, parentTime);
            } else if (layer.parentUnit == TimeUnit::Seconds && layer.contentUnit == TimeUnit::Beats) {
                local = tempo.beatsAtSeconds(parentTime) - tempo.beatsAtSeconds(layer.scheduleOrigin);
            }
        }
        double content = wrappedContent(layer, layer.playStart + local);
        if (layer.playStop > layer.playStart && content >= layer.playStop) {
            return false;
        }
        cursor = content;
        cursorUnit = layer.contentUnit;
    }
    if (hasWarp) {
        sourceSecond = warp.contentAt(cursor);
    } else if (cursorUnit == TimeUnit::Seconds) {
        sourceSecond = cursor;
    } else {
        sourceSecond = tempo.secondsAtBeat(cursor) - tempo.secondsAtBeat(0);
    }
    if (file.duration > 0 && (sourceSecond < -1.0e-4 || sourceSecond > file.duration + 1.0e-3)) {
        return false;
    }
    return sourceSecond >= -1.0e-4;
}

double AudioEvent::contentTime(const TempoMap& tempo, double arrangementSecond, int layerIndex) const {
    double cursor = arrangementSecond;
    TimeUnit cursorUnit = TimeUnit::Seconds;
    int seen = 0;
    for (const ClipLayer& layer : layers) {
        double parentTime = cursor;
        if (layer.parentUnit != cursorUnit) {
            parentTime = convertInto(tempo, cursor, cursorUnit, layer.parentUnit, cursor);
        }
        double local = parentTime - layer.scheduleOrigin;
        if (layer.parentUnit != layer.contentUnit) {
            if (layer.parentUnit == TimeUnit::Beats && layer.contentUnit == TimeUnit::Seconds) {
                local = tempo.secondsBetweenBeats(layer.scheduleOrigin, parentTime);
            } else if (layer.parentUnit == TimeUnit::Seconds && layer.contentUnit == TimeUnit::Beats) {
                local = tempo.beatsAtSeconds(parentTime) - tempo.beatsAtSeconds(layer.scheduleOrigin);
            }
        }
        cursor = wrappedContent(layer, layer.playStart + local);
        cursorUnit = layer.contentUnit;
        if (seen == layerIndex) {
            return cursor;
        }
        ++seen;
    }
    return cursor;
}

double Project::lengthSeconds() const {
    double length = 0;
    for (const AudioEvent& event : events) {
        length = std::max(length, event.endSecond);
    }
    return length;
}

bool Project::anySolo() const {
    for (const MixerChannel& channel : channels) {
        if (channel.solo && channel.audible) {
            return true;
        }
    }
    return false;
}

LoadResult loadProjectDocument(const XmlDocument& document, std::shared_ptr<AudioStore> store) {
    LoadResult result;
    result.store = std::move(store);
    if (!document.error.empty()) {
        result.error = document.error;
        return result;
    }
    if (document.root.name != "Project") {
        result.error = "Root element is not Project";
        return result;
    }
    NodeIndex index;
    index.add(document.root);
    if (const XmlNode* application = document.root.child("Application")) {
        result.project.application = application->attribute("name");
    }
    if (const XmlNode* transport = document.root.child("Transport")) {
        readTransport(*transport, result.project);
    } else {
        result.project.tempo.points.push_back(TempoPoint{});
        result.project.timeSignature.points.push_back(TimeSigPoint{});
    }
    std::unordered_map<std::string, int> trackChannel;
    std::unordered_map<std::string, ParameterBinding> parameters;
    if (const XmlNode* structure = document.root.child("Structure")) {
        for (const XmlNode& child : structure->children) {
            if (child.name == "Track") {
                readTracks(child, result.project, trackChannel, parameters);
            }
        }
    }
    linkDestinations(result.project);
    if (const XmlNode* arrangement = document.root.child("Arrangement")) {
        readArrangement(*arrangement, result.project, index, trackChannel, parameters);
    }
    markEqualPowerOuts(result.project);
    result.project.modelBytes = countModelBytes(result.project);
    return result;
}

LoadResult finishLoad(std::shared_ptr<AudioStore> store) {
    const std::string xml = store->projectXml();
    if (xml.empty()) {
        LoadResult result;
        result.error = "project.xml is missing from the archive";
        return result;
    }
    return loadProjectDocument(parseXml(xml), std::move(store));
}

LoadResult loadProjectFile(const std::string& path) {
    auto store = std::make_shared<AudioStore>();
    std::string error;
    if (!store->openFile(path, error)) {
        LoadResult result;
        result.error = error;
        return result;
    }
    return finishLoad(std::move(store));
}

LoadResult loadProjectBytes(const std::uint8_t* data, std::size_t size, const std::string& label) {
    auto store = std::make_shared<AudioStore>();
    std::string error;
    if (data == nullptr || !store->openMemory(data, size, label, error)) {
        LoadResult result;
        result.error = error.empty() ? "Empty project buffer" : error;
        return result;
    }
    return finishLoad(std::move(store));
}

}  // namespace dawplay
