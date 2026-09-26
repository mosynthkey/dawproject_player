#include "dawplay/timeline.hpp"

#include <algorithm>
#include <cmath>

namespace dawplay {
namespace {

constexpr double kMinBpm = 1.0;
constexpr double kPi = 3.14159265358979323846;

double clampBpm(double bpm) {
    return std::max(bpm, kMinBpm);
}

}  // namespace

double AutomationCurve::valueAt(double time) const {
    if (points.empty()) {
        return fallback;
    }
    if (time < points.front().time) {
        return fallback;
    }
    const AutomationPoint* previous = &points.front();
    for (size_t pointIndex = 1; pointIndex < points.size(); ++pointIndex) {
        const AutomationPoint& next = points[pointIndex];
        if (time < next.time) {
            if (!previous->linear || next.time <= previous->time) {
                return previous->value;
            }
            const double blend = (time - previous->time) / (next.time - previous->time);
            return previous->value + (next.value - previous->value) * blend;
        }
        previous = &next;
    }
    return points.back().value;
}

double TempoMap::bpmAt(double beats) const {
    if (points.empty()) {
        return 120.0;
    }
    if (beats < points.front().beats) {
        return clampBpm(points.front().bpm);
    }
    const TempoPoint* previous = &points.front();
    for (size_t pointIndex = 1; pointIndex < points.size(); ++pointIndex) {
        const TempoPoint& next = points[pointIndex];
        if (beats < next.beats) {
            if (!previous->linear || next.beats <= previous->beats) {
                return clampBpm(previous->bpm);
            }
            const double blend = (beats - previous->beats) / (next.beats - previous->beats);
            return clampBpm(previous->bpm + (next.bpm - previous->bpm) * blend);
        }
        previous = &next;
    }
    return clampBpm(points.back().bpm);
}

double TempoMap::secondsBetweenBeats(double beatStart, double beatEnd) const {
    if (beatEnd < beatStart) {
        return -secondsBetweenBeats(beatEnd, beatStart);
    }
    if (!(beatEnd > beatStart)) {
        return 0;
    }
    if (points.empty()) {
        return (beatEnd - beatStart) * 60.0 / 120.0;
    }

    std::vector<double> marks;
    marks.push_back(beatStart);
    for (const TempoPoint& point : points) {
        if (point.beats > beatStart && point.beats < beatEnd) {
            marks.push_back(point.beats);
        }
    }
    marks.push_back(beatEnd);

    double seconds = 0;
    for (size_t markIndex = 0; markIndex + 1 < marks.size(); ++markIndex) {
        const double left = marks[markIndex];
        const double right = marks[markIndex + 1];
        const double span = right - left;
        if (span <= 0) {
            continue;
        }
        if (left < points.front().beats) {
            seconds += span * 60.0 / clampBpm(points.front().bpm);
            continue;
        }
        size_t owner = 0;
        for (size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
            if (points[pointIndex].beats <= left + 1.0e-9) {
                owner = pointIndex;
            }
        }
        const bool ramps = points[owner].linear && owner + 1 < points.size();
        if (!ramps) {
            seconds += span * 60.0 / clampBpm(points[owner].bpm);
            continue;
        }
        const double beat0 = points[owner].beats;
        const double beat1 = points[owner + 1].beats;
        const double bpmStart = clampBpm(points[owner].bpm);
        const double bpmEnd = clampBpm(points[owner + 1].bpm);
        const auto bpmAtBeat = [&](double beat) {
            if (beat1 <= beat0) {
                return bpmStart;
            }
            const double blend = (beat - beat0) / (beat1 - beat0);
            return clampBpm(bpmStart + (bpmEnd - bpmStart) * blend);
        };
        const double y0 = bpmAtBeat(left);
        const double y1 = bpmAtBeat(right);
        if (std::abs(y1 - y0) < 1.0e-9) {
            seconds += span * 60.0 / y0;
        } else {
            // 60/bpm integrated across a linear tempo ramp.
            seconds += 60.0 * span / (y1 - y0) * std::log(y1 / y0);
        }
    }
    return seconds;
}

double TempoMap::secondsAtBeat(double beats) const {
    return secondsBetweenBeats(0, beats);
}

double TempoMap::beatsAtSeconds(double seconds) const {
    if (seconds <= 0) {
        return 0;
    }
    if (points.empty()) {
        return seconds * 120.0 / 60.0;
    }
    // secondsAtBeat is monotonic for positive tempos, so a binary search is enough.
    double low = points.front().beats - 64.0;
    double high = points.back().beats + 64.0;
    const double tailBpm = clampBpm(points.back().bpm);
    if (seconds > secondsAtBeat(high)) {
        high += (seconds - secondsAtBeat(high)) * tailBpm / 60.0 + 8.0;
    }
    if (seconds < secondsAtBeat(low)) {
        const double headBpm = clampBpm(points.front().bpm);
        low -= (secondsAtBeat(low) - seconds) * headBpm / 60.0 + 8.0;
    }
    for (int step = 0; step < 48; ++step) {
        const double mid = 0.5 * (low + high);
        if (secondsAtBeat(mid) < seconds) {
            low = mid;
        } else {
            high = mid;
        }
    }
    return 0.5 * (low + high);
}

namespace {

struct SigRegion {
    double startBeat = 0;
    double startBar = 1;
    double barBeats = 4;
    double endBeat = 1.0e300;
};

std::vector<SigRegion> regionsOf(const TimeSigMap& map) {
    std::vector<TimeSigPoint> points = map.points;
    if (points.empty()) {
        points.push_back(TimeSigPoint{});
    }
    std::sort(points.begin(), points.end(), [](const TimeSigPoint& left, const TimeSigPoint& right) {
        return left.beats < right.beats;
    });
    std::vector<SigRegion> regions;
    double bar = 1.0;
    for (size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex) {
        SigRegion region;
        region.startBeat = points[pointIndex].beats;
        region.startBar = bar;
        const int numerator = std::max(points[pointIndex].numerator, 1);
        const int denominator = std::max(points[pointIndex].denominator, 1);
        // A "beat" in the file is a quarter note. A 6/8 bar is 3 quarters long.
        region.barBeats = static_cast<double>(numerator) * 4.0 / static_cast<double>(denominator);
        if (pointIndex + 1 < points.size()) {
            region.endBeat = points[pointIndex + 1].beats;
            bar += (region.endBeat - region.startBeat) / region.barBeats;
        }
        regions.push_back(region);
    }
    return regions;
}

}  // namespace

double TimeSigMap::beatsAtBar(double bar) const {
    const std::vector<SigRegion> regions = regionsOf(*this);
    const SigRegion* chosen = &regions.front();
    for (const SigRegion& region : regions) {
        const double regionEndBar = region.startBar + (region.endBeat - region.startBeat) / region.barBeats;
        if (bar >= region.startBar && (bar < regionEndBar || region.endBeat > 1.0e299)) {
            chosen = &region;
            if (bar < regionEndBar) {
                break;
            }
        }
    }
    return chosen->startBeat + (bar - chosen->startBar) * chosen->barBeats;
}

double TimeSigMap::barAtBeats(double beats) const {
    const std::vector<SigRegion> regions = regionsOf(*this);
    const SigRegion* chosen = &regions.front();
    for (const SigRegion& region : regions) {
        if (beats >= region.startBeat) {
            chosen = &region;
        }
    }
    return chosen->startBar + (beats - chosen->startBeat) / chosen->barBeats;
}

double WarpMap::contentAt(double time) const {
    if (points.empty()) {
        return time;
    }
    if (points.size() == 1 || time <= points.front().time) {
        if (points.size() == 1) {
            return points.front().contentTime;
        }
        const double span = points[1].time - points[0].time;
        if (std::abs(span) < 1.0e-12) {
            return points.front().contentTime;
        }
        const double slope = (points[1].contentTime - points[0].contentTime) / span;
        return points.front().contentTime + (time - points.front().time) * slope;
    }
    const WarpPoint* previous = &points.front();
    for (size_t pointIndex = 1; pointIndex < points.size(); ++pointIndex) {
        const WarpPoint& next = points[pointIndex];
        if (time <= next.time || pointIndex + 1 == points.size()) {
            const double span = next.time - previous->time;
            if (std::abs(span) < 1.0e-12) {
                return next.contentTime;
            }
            const double slope = (next.contentTime - previous->contentTime) / span;
            return previous->contentTime + (time - previous->time) * slope;
        }
        previous = &next;
    }
    return points.back().contentTime;
}

const char* stretchPresetName(StretchPreset preset) {
    return preset == StretchPreset::Default ? "default" : "cheaper";
}

double linearToDecibelGain(double decibels) {
    return std::pow(10.0, decibels / 20.0);
}

float fadeGain(float position, bool equalPower) {
    const float clamped = std::clamp(position, 0.0f, 1.0f);
    if (equalPower) {
        return std::sin(clamped * static_cast<float>(kPi * 0.5));
    }
    return clamped;
}

}  // namespace dawplay
