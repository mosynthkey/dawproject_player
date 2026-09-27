#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace dawplay {

enum class TimeUnit { Beats, Seconds };

struct AutomationPoint {
    double time = 0;
    double value = 0;
    // Interpolation of the segment that starts at this point. The format
    // defaults an omitted interpolation to hold.
    bool linear = false;
};

struct AutomationCurve {
    std::vector<AutomationPoint> points;
    double fallback = 0;
    // Clock of `points`. Clip expression curves are evaluated in content time instead.
    TimeUnit timeUnit = TimeUnit::Beats;

    double valueAt(double time) const;
};

struct TempoPoint {
    double beats = 0;
    double bpm = 120;
    bool linear = false;
};

struct TempoMap {
    std::vector<TempoPoint> points;

    double bpmAt(double beats) const;
    double secondsAtBeat(double beats) const;
    double beatsAtSeconds(double seconds) const;
    double secondsBetweenBeats(double beatStart, double beatEnd) const;

private:
    // Integrated once from `points`. Queries only binary-search this table.
    struct Span {
        double beat0 = 0;
        double beat1 = 0;
        double second0 = 0;
        double bpm0 = 120;
        double bpm1 = 120;
        bool ramp = false;
    };
    mutable std::vector<Span> spans_;
    mutable std::size_t cachedPoints_ = static_cast<std::size_t>(-1);
    mutable double cachedTailBeat_ = 0;
    mutable double cachedTailBpm_ = 0;
    void ensureSpans() const;
    double secondsFromZero(double beats) const;
    static double partialSeconds(const Span& span, double beatEnd);
};

struct TimeSigPoint {
    double beats = 0;
    int numerator = 4;
    int denominator = 4;
};

struct TimeSigMap {
    std::vector<TimeSigPoint> points;

    // Bar 1 is the start of the arrangement.
    double beatsAtBar(double bar) const;
    double barAtBeats(double beats) const;

private:
    struct Region {
        double startBeat = 0;
        double startBar = 1;
        double barBeats = 4;
        double endBeat = 1.0e300;
    };
    mutable std::vector<Region> regions_;
    mutable std::size_t cachedPoints_ = static_cast<std::size_t>(-1);
    mutable double cachedTailBeat_ = 0;
    void ensureRegions() const;
};

struct WarpPoint {
    double time = 0;
    double contentTime = 0;
};

struct WarpMap {
    std::vector<WarpPoint> points;

    double contentAt(double time) const;
    bool empty() const { return points.size() < 2; }
};

enum class StretchPreset { Cheaper, Default };

const char* stretchPresetName(StretchPreset preset);

double linearToDecibelGain(double decibels);
float fadeGain(float position, bool equalPower);

}  // namespace dawplay
