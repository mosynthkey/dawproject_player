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
