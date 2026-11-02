#include "FormantTracker.hpp"

#include "Lpc.hpp"

#include <MayaFlux/Kinesis/Discrete/Analysis.hpp>
#include <MayaFlux/Kinesis/Discrete/Coefficients.hpp>
#include <MayaFlux/Kinesis/Discrete/Convolution.hpp>
#include <MayaFlux/Kinesis/Discrete/Taper.hpp>

namespace MayaFlux::Kinesis {

namespace {

    /** @brief Length of the low-pass used before decimating. */
    constexpr size_t k_kernel_taps = 63;

    /** @brief Share of the new rate's Nyquist where the low-pass cuts off. */
    constexpr double k_cutoff_share = 0.9;

    /** @brief Added to the zero-lag autocorrelation so the recursion stays stable. */
    constexpr double k_white_noise = 1.0001;

}

FormantTracker::FormantTracker(FormantConfig config)
    : m_config(config)
    , m_factor(std::max<size_t>(1, static_cast<size_t>(std::lround(config.sample_rate / config.analysis_rate))))
    , m_rate(config.sample_rate / static_cast<double>(m_factor))
    , m_kernel(Discrete::sinc_lowpass(k_cutoff_share * m_rate * 0.5, config.sample_rate, k_kernel_taps))
    , m_gate(config.gate)
    , m_smoothers { HampelFilter<double>(config.smoothing_window), HampelFilter<double>(config.smoothing_window), HampelFilter<double>(config.smoothing_window) }
{
}

FormantReading FormantTracker::analyse(std::span<const double> frame, double hop_seconds)
{
    FormantReading reading;
    measure(frame, reading);

    auto signal = decimate(frame);
    const double hop = hop_seconds > 0.0 ? hop_seconds : 512.0 / m_config.sample_rate;
    reading.activity = m_gate.update(reading.rms, periodicity(signal), hop);
    reading.voiced = reading.activity.voice && reading.rms >= m_config.gate_rms && reading.zcr <= m_config.gate_zcr;

    if (reading.voiced) {
        pre_emphasise(signal);
        Discrete::apply_hann(signal);

        std::vector<double> r = Discrete::auto_correlate(signal, false);
        if (!r.empty()) {
            r[0] *= k_white_noise;
        }
        m_envelope = Discrete::lpc_envelope_db(Discrete::levinson_durbin(r, m_config.lpc_order), m_config.grid_points);
        assign(Discrete::spectral_peaks(m_envelope, 0.5 * m_rate));
    }

    reading.f1 = m_last[0];
    reading.f2 = m_last[1];
    reading.f3 = m_last[2];
    return reading;
}

void FormantTracker::measure(std::span<const double> frame, FormantReading& reading) const
{
    const auto size = static_cast<uint32_t>(frame.size());
    const size_t windows = Discrete::num_windows(frame.size(), size, size);
    if (windows == 0) {
        return;
    }
    reading.rms = Discrete::rms(frame, windows, size, size).front();
    reading.zcr = Discrete::zero_crossing_rate(frame, windows, size, size).front();
    reading.peak = Discrete::peak(frame, windows, size, size).front();
}

std::vector<double> FormantTracker::decimate(std::span<const double> frame) const
{
    const std::vector<double> filtered = Discrete::convolve(frame, m_kernel);
    std::vector<double> kept;
    kept.reserve(filtered.size() / m_factor + 1);
    for (size_t i = 0; i < filtered.size(); i += m_factor) {
        kept.push_back(filtered[i]);
    }
    return kept;
}

double FormantTracker::periodicity(std::span<const double> signal) const
{
    const std::vector<double> r = Discrete::auto_correlate(signal, true);

    // The correlation of a short frame falls off as (N - lag) / N, so divide that out. Stop at half the
    // frame, where too few samples overlap to trust the value.
    const size_t n = signal.size();
    const auto first = static_cast<size_t>(m_rate / m_config.pitch_max_hz);
    const size_t last = std::min(static_cast<size_t>(m_rate / m_config.pitch_min_hz), n / 2);

    double best = 0.0;
    for (size_t lag = std::max<size_t>(first, 1); lag <= last && lag < r.size(); ++lag) {
        best = std::max(best, r[lag] * static_cast<double>(n) / static_cast<double>(n - lag));
    }
    return std::clamp(best, 0.0, 1.0);
}

void FormantTracker::pre_emphasise(std::vector<double>& signal) const
{
    for (size_t i = signal.size(); i-- > 1;) {
        signal[i] -= m_config.pre_emphasis * signal[i - 1];
    }
}

void FormantTracker::assign(std::span<const double> peak_hz)
{
    double floor = 0.0;
    for (size_t i = 0; i < m_last.size(); ++i) {
        const auto [low, high] = m_config.bands[i];
        const double lowest = std::max(low, floor);
        const auto hit = std::ranges::find_if(peak_hz, [&](double hz) { return hz > lowest && hz < high; });
        if (hit == peak_hz.end()) {
            continue;
        }
        m_last[i] = m_smoothers[i].accept(*hit);
        floor = *hit;
    }
}

} // namespace MayaFlux::Kinesis
