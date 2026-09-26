// spectrogram(): clips drawn on one time axis, one frequency axis and one dB
// colour scale (magma), with tick marks and labels in a 5x7 bitmap font. See
// ear.h for the options.
//
// Each plot column covers [x, x+1) / width of the shared duration and shows
// the per-bin maximum over the STFT frames centred in it (the nearest frame
// when none is); each row shows the maximum over the bins its frequency span
// covers, or the dB interpolated between the two nearest bins when the span is
// narrower than a bin. A shorter clip's panel is hatched past its end.

#include "ear_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace broaudio::ear {

using namespace detail;

namespace {

struct Rgb {
    uint8_t r, g, b;
};

constexpr Rgb kBackground{18, 18, 22};
constexpr Rgb kText{224, 224, 228};
constexpr Rgb kTick{190, 190, 198};
constexpr Rgb kBorder{96, 96, 108};
constexpr Rgb kPastEnd{34, 34, 42};
constexpr Rgb kHatch{64, 64, 76};

Rgb magma(double v) {
    static constexpr Rgb stops[] = {
        {0, 0, 4},       {28, 16, 68},    {79, 18, 123},   {129, 37, 129}, {181, 54, 122},
        {229, 80, 100},  {251, 135, 97},  {254, 194, 135}, {252, 253, 191},
    };
    constexpr int n = static_cast<int>(sizeof(stops) / sizeof(stops[0]));
    v = std::clamp(v, 0.0, 1.0) * (n - 1);
    const int i = std::min(n - 2, static_cast<int>(v));
    const double f = v - i;
    auto mix = [f](uint8_t a, uint8_t b) {
        return static_cast<uint8_t>(std::lround(a + (static_cast<double>(b) - a) * f));
    };
    return {mix(stops[i].r, stops[i + 1].r), mix(stops[i].g, stops[i + 1].g),
            mix(stops[i].b, stops[i + 1].b)};
}

struct Canvas {
    int w = 0, h = 0;
    std::vector<uint8_t>& px;
    void set(int x, int y, Rgb c) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint8_t* p = px.data() + (static_cast<size_t>(y) * w + x) * 4;
        p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 255;
    }
    void fill(int x0, int y0, int x1, int y1, Rgb c) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x) set(x, y, c);
    }
    int textWidth(const std::string& s, int scale) const {
        return s.empty() ? 0 : static_cast<int>(s.size()) * 6 * scale - scale;
    }
    void text(int x, int y, const std::string& s, int scale, Rgb c) {
        for (char ch : s) {
            const uint8_t* rows = glyph5x7(ch);
            for (int r = 0; r < 7; ++r)
                for (int col = 0; col < 5; ++col)
                    if (rows[r] & (1u << (4 - col)))
                        fill(x + col * scale, y + r * scale, x + (col + 1) * scale, y + (r + 1) * scale, c);
            x += 6 * scale;
        }
    }
};

// Shortest decimal for `v` with at most `maxDecimals` places.
std::string formatNumber(double v, int maxDecimals) {
    char buf[64];
    for (int d = 0; d <= maxDecimals; ++d) {
        const double p = std::pow(10.0, d);
        if (std::fabs(std::round(v * p) - v * p) < 1e-6 || d == maxDecimals) {
            std::snprintf(buf, sizeof(buf), "%.*f", d, v);
            std::string s = buf;
            if (s == "-0") s = "0";
            return s;
        }
    }
    return "0";
}

std::string formatHz(double hz) {
    return hz >= 1000.0 ? formatNumber(hz / 1000.0, 1) + "k" : formatNumber(hz, 0);
}

// A 1-2-2.5-5 step giving at most `maxTicks` ticks over `span`.
double niceStep(double span, int maxTicks) {
    const double raw = span / std::max(1, maxTicks);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0}) {
        if (m * mag >= raw - 1e-12) return m * mag;
    }
    return 10.0 * mag;
}

struct ClipSpec {
    Stft stft;
    std::vector<float> db;  // frames x bins
    double duration = 0;
};

struct FreqAxis {
    FrequencyScale scale;
    double lo, hi;
    // v in [0, 1] from the bottom of the plot.
    double hz(double v) const {
        switch (scale) {
        case FrequencyScale::Log: return lo * std::pow(hi / lo, v);
        case FrequencyScale::Mel: return melToHz(hzToMel(lo) + v * (hzToMel(hi) - hzToMel(lo)));
        default: return lo + v * (hi - lo);
        }
    }
    double v(double hz) const {
        switch (scale) {
        case FrequencyScale::Log: return std::log(hz / lo) / std::log(hi / lo);
        case FrequencyScale::Mel: return (hzToMel(hz) - hzToMel(lo)) / (hzToMel(hi) - hzToMel(lo));
        default: return (hz - lo) / (hi - lo);
        }
    }
};

void drawPanel(Canvas& cv, const ClipSpec& spec, const SpectrogramPanel& p, const FreqAxis& fa,
               double totalDur, double minDb, double dbRange) {
    const Stft& s = spec.stft;
    std::vector<float> col(s.bins);
    for (int x = 0; x < p.width; ++x) {
        const double t0 = totalDur * x / p.width, t1 = totalDur * (x + 1) / p.width;
        if (t0 >= spec.duration) {
            for (int y = 0; y < p.height; ++y) {
                cv.set(p.x + x, p.y + y, ((p.x + x + p.y + y) % 8) < 2 ? kHatch : kPastEnd);
            }
            continue;
        }
        std::fill(col.begin(), col.end(), -300.0f);
        const int f0 = std::max(0, static_cast<int>(std::ceil(t0 / s.hopSec - 1e-9)));
        bool any = false;
        for (int f = f0; f < s.frames && s.time(f) < t1; ++f) {
            const float* d = spec.db.data() + static_cast<size_t>(f) * s.bins;
            for (int k = 0; k < s.bins; ++k) col[k] = std::max(col[k], d[k]);
            any = true;
        }
        if (!any) {
            const int f = std::clamp(static_cast<int>(std::lround(0.5 * (t0 + t1) / s.hopSec)), 0, s.frames - 1);
            const float* d = spec.db.data() + static_cast<size_t>(f) * s.bins;
            std::copy(d, d + s.bins, col.begin());
        }
        for (int y = 0; y < p.height; ++y) {
            const double vLo = static_cast<double>(p.height - 1 - y) / p.height;
            const double vHi = static_cast<double>(p.height - y) / p.height;
            const double kLo = fa.hz(vLo) / s.binHz, kHi = fa.hz(vHi) / s.binHz;
            double db;
            if (kHi - kLo <= 1.0) {
                const double kc = std::clamp(0.5 * (kLo + kHi), 0.0, static_cast<double>(s.bins - 1));
                const int k0 = static_cast<int>(kc);
                const int k1 = std::min(s.bins - 1, k0 + 1);
                db = col[k0] + (col[k1] - col[k0]) * (kc - k0);
            } else {
                const int a = std::clamp(static_cast<int>(std::ceil(kLo)), 0, s.bins - 1);
                const int b = std::clamp(static_cast<int>(std::floor(kHi)), a, s.bins - 1);
                db = col[a];
                for (int k = a + 1; k <= b; ++k) db = std::max(db, static_cast<double>(col[k]));
            }
            cv.set(p.x + x, p.y + y, magma((db - minDb) / dbRange));
        }
    }
}

void drawAxes(Canvas& cv, const SpectrogramPanel& p, const FreqAxis& fa, double totalDur, int fs) {
    const int charH = 7 * fs;
    // Border.
    for (int x = p.x - 1; x <= p.x + p.width; ++x) {
        cv.set(x, p.y - 1, kBorder);
        cv.set(x, p.y + p.height, kBorder);
    }
    for (int y = p.y - 1; y <= p.y + p.height; ++y) {
        cv.set(p.x - 1, y, kBorder);
        cv.set(p.x + p.width, y, kBorder);
    }
    // Frequency ticks.
    std::vector<double> ticks;
    if (fa.scale == FrequencyScale::Linear) {
        const double step = niceStep(fa.hi - fa.lo, std::max(2, p.height / (charH * 3)));
        for (double f = std::ceil(fa.lo / step) * step; f <= fa.hi + 1e-9; f += step) ticks.push_back(f);
    } else {
        for (double f : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
            if (f >= fa.lo - 1e-9 && f <= fa.hi + 1e-9) ticks.push_back(f);
        }
    }
    int lastLabelY = 1 << 30;
    for (double f : ticks) {
        const int y = p.y + p.height - 1 - static_cast<int>(std::lround(fa.v(f) * (p.height - 1)));
        for (int x = p.x - 6; x < p.x - 1; ++x) cv.set(x, y, kTick);
        if (lastLabelY - y < charH + 2) continue;  // no overlapping labels
        const std::string label = formatHz(f);
        cv.text(p.x - 8 - cv.textWidth(label, fs), std::clamp(y - charH / 2, p.y - charH / 2, p.y + p.height - charH / 2),
                label, fs, kText);
        lastLabelY = y;
    }
    cv.text(p.x - 8 - cv.textWidth("Hz", fs), p.y - charH - 5, "Hz", fs, kText);
    // Time ticks.
    const int labelChars = 5;
    const double step = niceStep(totalDur, std::max(2, p.width / (labelChars * 6 * fs + 12)));
    int decimals = 0;
    while (decimals < 4 && std::fabs(std::round(step * std::pow(10.0, decimals)) - step * std::pow(10.0, decimals)) > 1e-6) {
        ++decimals;
    }
    for (int i = 0;; ++i) {
        const double t = i * step;
        if (t > totalDur + 1e-9) break;
        const int x = p.x + std::min(p.width - 1, static_cast<int>(std::lround(t / totalDur * p.width)));
        for (int y = p.y + p.height + 1; y < p.y + p.height + 6; ++y) cv.set(x, y, kTick);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, t);
        const std::string label = buf;
        const int w = cv.textWidth(label, fs);
        cv.text(std::clamp(x - w / 2, p.x - 4, p.x + p.width - w + 4), p.y + p.height + 8, label, fs, kText);
    }
    cv.text(p.x + p.width + 4, p.y + p.height + 8, "s", fs, kText);
}

void drawColorbar(Canvas& cv, int x, int yTop, int yBottom, double minDb, double maxDb, int fs) {
    const int charH = 7 * fs;
    const int barW = 14;
    const int h = std::max(1, yBottom - yTop);
    for (int y = 0; y < h; ++y) {
        const Rgb c = magma(1.0 - static_cast<double>(y) / std::max(1, h - 1));
        for (int i = 0; i < barW; ++i) cv.set(x + i, yTop + y, c);
    }
    const double range = maxDb - minDb;
    const double step = range <= 60.0 ? 10.0 : 20.0;
    for (double d = std::floor(maxDb / step) * step; d >= minDb - 1e-9; d -= step) {
        const int y = yTop + static_cast<int>(std::lround((maxDb - d) / range * (h - 1)));
        for (int i = barW; i < barW + 4; ++i) cv.set(x + i, y, kTick);
        cv.text(x + barW + 6, std::clamp(y - charH / 2, yTop - charH / 2, yBottom - charH / 2),
                formatNumber(d, 0), fs, kText);
    }
    cv.text(x, yTop - charH - 5, "dB", fs, kText);
}

} // namespace

SpectrogramImage spectrogram(const std::vector<const Clip*>& clips, const std::vector<std::string>& labels,
                             const SpectrogramOptions& opts) {
    SpectrogramImage img;
    std::vector<const Clip*> list;
    for (const Clip* c : clips) {
        if (c && c->sampleRate > 0) list.push_back(c);
    }
    if (list.empty()) return img;

    const int pw = std::clamp(opts.width, 32, 8192);
    const int ph = std::clamp(opts.height, 32, 4096);
    const int fs = std::clamp(opts.fontScale, 1, 4);
    const double dbRange = opts.dbRange > 1.0 ? opts.dbRange : 80.0;

    double totalDur = 0.0, minNyq = 1e12;
    for (const Clip* c : list) {
        totalDur = std::max(totalDur, c->duration());
        minNyq = std::min(minNyq, c->sampleRate / 2.0);
    }
    if (!(totalDur > 0.0)) totalDur = 1e-3;
    FreqAxis fa;
    fa.scale = opts.scale;
    fa.hi = opts.maxHz > 0.0 ? std::min(opts.maxHz, minNyq) : minNyq;
    fa.lo = opts.scale == FrequencyScale::Linear ? 0.0 : std::clamp(opts.minHz, 1.0, fa.hi * 0.5);

    // Spectra, and the shared top of the colour scale.
    std::vector<ClipSpec> specs(list.size());
    double loudest = -300.0;
    for (size_t i = 0; i < list.size(); ++i) {
        const Clip& c = *list[i];
        const int nfft = opts.fftSize > 0 ? std::clamp(nextPow2(opts.fftSize), 64, 32768) : defaultFftSize(c.sampleRate);
        const int perColumn = static_cast<int>(c.sampleRate * totalDur / pw);
        const int hop = std::max(1, std::min(nfft / 4, perColumn));
        specs[i].stft = computeStft(c, nfft, hop);
        specs[i].duration = c.duration();
        const Stft& s = specs[i].stft;
        specs[i].db.resize(s.power.size());
        for (size_t j = 0; j < s.power.size(); ++j) {
            const float d = static_cast<float>(10.0 * std::log10(static_cast<double>(s.power[j]) + 1e-20));
            specs[i].db[j] = d;
            loudest = std::max(loudest, static_cast<double>(d));
        }
    }
    const double maxDb = std::isnan(opts.maxDb) ? std::max(loudest, -120.0 + dbRange) : opts.maxDb;
    const double minDb = maxDb - dbRange;

    // Layout.
    const int charW = 6 * fs, charH = 7 * fs;
    const int left = charW * 4 + 14;
    const int top = charH + 10;
    const int bottom = charH + 14;
    const int right = 16 + 14 + 6 + charW * 4 + 8;
    const int n = static_cast<int>(list.size());
    if (opts.stacked) {
        img.width = left + pw + right;
        img.height = n * (top + ph + bottom);
    } else {
        img.width = n * (left + pw) + right;
        img.height = top + ph + bottom;
    }
    img.rgba.assign(static_cast<size_t>(img.width) * img.height * 4, 0);
    Canvas cv{img.width, img.height, img.rgba};
    cv.fill(0, 0, img.width, img.height, kBackground);

    for (int i = 0; i < n; ++i) {
        SpectrogramPanel p;
        p.label = i < static_cast<int>(labels.size()) && !labels[i].empty() ? labels[i] : "clip " + std::to_string(i + 1);
        p.width = pw;
        p.height = ph;
        p.x = opts.stacked ? left : i * (left + pw) + left;
        p.y = opts.stacked ? i * (top + ph + bottom) + top : top;
        p.duration = specs[i].duration;
        p.sampleRate = list[i]->sampleRate;
        drawPanel(cv, specs[i], p, fa, totalDur, minDb, dbRange);
        drawAxes(cv, p, fa, totalDur, fs);
        const std::string title = p.label + "  " + formatNumber(p.duration, 2) + " s";
        cv.text(p.x, p.y - charH - 5, title, fs, kText);
        img.panels.push_back(std::move(p));
    }
    const SpectrogramPanel& first = img.panels.front();
    const SpectrogramPanel& last = img.panels.back();
    drawColorbar(cv, img.width - right + 16, first.y, last.y + last.height, minDb, maxDb, fs);

    img.duration = totalDur;
    img.minHz = fa.lo;
    img.maxHz = fa.hi;
    img.minDb = minDb;
    img.maxDb = maxDb;
    return img;
}

} // namespace broaudio::ear
