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
    if (points.empty() || time < points.front().time) {
        return fallback;
    }
    const auto upper = std::upper_bound(points.begin(), points.end(), time,
                                         [](double query, const AutomationPoint& point) { return query < point.time; });
    if (upper == points.end()) {
        return points.back().value;
    }
    const AutomationPoint& previous = *(upper - 1);
    const AutomationPoint& next = *upper;
    if (!previous.linear || next.time <= previous.time) {
        return previous.value;
    }
    const double blend = (time - previous.time) / (next.time - previous.time);
    return previous.value + (next.value - previous.value) * blend;
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

double TempoMap::partialSeconds(const Span& span, double beatEnd) {
    const double end = std::min(beatEnd, span.beat1);
    const double spanBeats = end - span.beat0;
    if (!(spanBeats > 0)) {
        return 0;
    }
    const double y0 = span.bpm0;
    const double beatSpan = span.beat1 - span.beat0;
    if (!span.ramp || !(beatSpan > 0) || std::abs(span.bpm1 - span.bpm0) < 1.0e-9) {
        return spanBeats * 60.0 / y0;
    }
    const double y1 = span.bpm0 + (span.bpm1 - span.bpm0) * (spanBeats / beatSpan);
    if (std::abs(y1 - y0) < 1.0e-9) {
        return spanBeats * 60.0 / y0;
    }
    // 60/bpm integrated across a linear tempo ramp.
    return 60.0 * spanBeats / (y1 - y0) * std::log(y1 / y0);
}

void TempoMap::ensureSpans() const {
    const double tailBeat = points.empty() ? 0 : points.back().beats;
    const double tailBpm = points.empty() ? 120 : points.back().bpm;
    if (cachedPoints_ == points.size() && cachedTailBeat_ == tailBeat && cachedTailBpm_ == tailBpm && !spans_.empty()) {
        return;
    }
    cachedPoints_ = points.size();
    cachedTailBeat_ = tailBeat;
    cachedTailBpm_ = tailBpm;
    spans_.clear();

    const double headBpm = clampBpm(points.empty() ? 120 : points.front().bpm);
    const double firstBeat = points.empty() ? 0 : points.front().beats;
    const double headBeat = std::min(0.0, firstBeat) - 1.0e6;
    Span head;
    head.beat0 = headBeat;
    head.beat1 = firstBeat;
    head.bpm0 = headBpm;
    head.bpm1 = headBpm;
    head.ramp = false;
    if (head.beat1 > head.beat0) {
        spans_.push_back(head);
    }
    for (size_t pointIndex = 0; pointIndex + 1 < points.size(); ++pointIndex) {
        Span span;
        span.beat0 = points[pointIndex].beats;
        span.beat1 = points[pointIndex + 1].beats;
        if (!(span.beat1 > span.beat0)) {
            continue;
        }
        span.bpm0 = clampBpm(points[pointIndex].bpm);
        span.bpm1 = clampBpm(points[pointIndex + 1].bpm);
        span.ramp = points[pointIndex].linear;
        spans_.push_back(span);
    }
    Span tail;
    tail.beat0 = firstBeat;
    if (!points.empty()) {
        tail.beat0 = points.back().beats;
    }
    tail.beat1 = 1.0e300;
    tail.bpm0 = clampBpm(points.empty() ? 120 : points.back().bpm);
    tail.bpm1 = tail.bpm0;
    tail.ramp = false;
    spans_.push_back(tail);

    double fromLeft = 0;
    for (Span& span : spans_) {
        span.second0 = fromLeft;
        fromLeft += partialSeconds(span, span.beat1);
    }
    const double atZero = secondsFromZero(0);
    for (Span& span : spans_) {
        span.second0 -= atZero;
    }
}

double TempoMap::secondsFromZero(double beats) const {
    if (spans_.empty()) {
        return beats * 60.0 / 120.0;
    }
    const Span* chosen = &spans_.front();
    size_t low = 0;
    size_t high = spans_.size();
    while (low + 1 < high) {
        const size_t mid = low + (high - low) / 2;
        if (spans_[mid].beat0 <= beats) {
            low = mid;
        } else {
            high = mid;
        }
    }
    chosen = &spans_[low];
    return chosen->second0 + partialSeconds(*chosen, beats);
}

double TempoMap::secondsBetweenBeats(double beatStart, double beatEnd) const {
    ensureSpans();
    return secondsFromZero(beatEnd) - secondsFromZero(beatStart);
}

double TempoMap::secondsAtBeat(double beats) const {
    ensureSpans();
    return secondsFromZero(beats);
}

double TempoMap::beatsAtSeconds(double seconds) const {
    ensureSpans();
    if (seconds <= 0) {
        return 0;
    }
    const Span* chosen = &spans_.front();
    size_t low = 0;
    size_t high = spans_.size();
    while (low + 1 < high) {
        const size_t mid = low + (high - low) / 2;
        if (spans_[mid].second0 <= seconds) {
            low = mid;
        } else {
            high = mid;
        }
    }
    chosen = &spans_[low];
    const double into = seconds - chosen->second0;
    const double beatSpan = chosen->beat1 - chosen->beat0;
    const double bpmSpan = chosen->bpm1 - chosen->bpm0;
    if (!chosen->ramp || !(beatSpan > 0) || std::abs(bpmSpan) < 1.0e-9) {
        return chosen->beat0 + into * chosen->bpm0 / 60.0;
    }
    const double logRatio = into * bpmSpan / (60.0 * beatSpan);
    const double bpm = chosen->bpm0 * std::exp(logRatio);
    const double portion = std::clamp((bpm - chosen->bpm0) / bpmSpan, 0.0, 1.0);
    return chosen->beat0 + portion * beatSpan;
}

void TimeSigMap::ensureRegions() const {
    const double tailBeat = points.empty() ? 0 : points.back().beats;
    if (cachedPoints_ == points.size() && cachedTailBeat_ == tailBeat && !regions_.empty()) {
        return;
    }
    cachedPoints_ = points.size();
    cachedTailBeat_ = tailBeat;
    std::vector<TimeSigPoint> sorted = points;
    if (sorted.empty()) {
        sorted.push_back(TimeSigPoint{});
    }
    std::sort(sorted.begin(), sorted.end(), [](const TimeSigPoint& left, const TimeSigPoint& right) {
        return left.beats < right.beats;
    });
    regions_.clear();
    double bar = 1.0;
    for (size_t pointIndex = 0; pointIndex < sorted.size(); ++pointIndex) {
        Region region;
        region.startBeat = sorted[pointIndex].beats;
        region.startBar = bar;
        const int numerator = std::max(sorted[pointIndex].numerator, 1);
        const int denominator = std::max(sorted[pointIndex].denominator, 1);
        // A "beat" in the file is a quarter note. A 6/8 bar is 3 quarters long.
        region.barBeats = static_cast<double>(numerator) * 4.0 / static_cast<double>(denominator);
        if (pointIndex + 1 < sorted.size()) {
            region.endBeat = sorted[pointIndex + 1].beats;
            bar += (region.endBeat - region.startBeat) / region.barBeats;
        }
        regions_.push_back(region);
    }
}

double TimeSigMap::beatsAtBar(double bar) const {
    ensureRegions();
    const Region* chosen = &regions_.front();
    for (const Region& region : regions_) {
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
    ensureRegions();
    const Region* chosen = &regions_.front();
    for (const Region& region : regions_) {
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
    const auto upper = std::upper_bound(points.begin(), points.end(), time,
                                         [](double query, const WarpPoint& point) { return query < point.time; });
    const WarpPoint& next = upper == points.end() ? points.back() : *upper;
    const WarpPoint& previous = upper == points.begin() ? points.front() : *(upper - 1);
    const double span = next.time - previous.time;
    if (std::abs(span) < 1.0e-12) {
        return next.contentTime;
    }
    const double slope = (next.contentTime - previous.contentTime) / span;
    return previous.contentTime + (time - previous.time) * slope;
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
