#pragma once

#include "VoiceGate.hpp"

#include <MayaFlux/Kinesis/HampelFilter.hpp>

namespace MayaFlux::Kinesis {

/**
 * @struct FormantConfig
 * @brief The numbers that tune a FormantTracker. Defaults suit a speaking voice at 48 kHz.
 */
struct FormantConfig {
    double sample_rate { 48000.0 };
    uint32_t frame_size { 1024 }; ///< Samples per analysis frame, before decimation.
    double analysis_rate { 12000.0 }; ///< Rate after decimation. The formants of speech sit below half of it.
    uint32_t lpc_order { 12 };
    uint32_t grid_points { 256 }; ///< Points of the envelope from 0 Hz to half the analysis rate.
    double pre_emphasis { 0.97 };
    double gate_rms { 0.003 }; ///< Frames quieter than this are never voiced, whatever the floor says.
    double gate_zcr { 0.10 }; ///< Frames that cross zero more often than this per sample are noise, not voice.
    double pitch_min_hz { 90.0 }; ///< Lowest pitch the pitchedness measure looks for.
    double pitch_max_hz { 400.0 }; ///< Highest.
    std::array<std::pair<double, double>, 3> bands { { { 200.0, 1000.0 }, { 500.0, 3000.0 }, { 1500.0, 3600.0 } } }; ///< Allowed Hz range of F1, F2, F3.
    size_t smoothing_window { 8 }; ///< Frames the Hampel filter looks back over.
    VoiceGateConfig gate; ///< The voice gate: noise floor, pitchedness threshold, voting.
};

/**
 * @struct FormantReading
 * @brief One reading. The formants hold their last values while the frame is not voiced.
 */
struct FormantReading {
    double f1 { 0.0 };
    double f2 { 0.0 };
    double f3 { 0.0 };
    VoiceActivity activity; ///< What the voice gate decided and why.
    double rms { 0.0 };
    double zcr { 0.0 }; ///< Zero crossings per sample of the frame.
    double peak { 0.0 }; ///< Largest absolute sample of the frame. Exactly zero means the input is silent.
    bool voiced { false };
};

/**
 * @class FormantTracker
 * @brief Follows the first three formants (F1, F2, F3) of a voice, frame by frame.
 *
 * A pure analysis chain with no buffers and no threads: give it a frame, get a reading. One frame goes
 * through these stages, in this order. Each is a small private function.
 *
 *   1. measure      RMS, zero-crossing rate and peak of the frame.
 *   2. decimate     Low-pass and keep every Nth sample, so the formants sit in a low band.
 *   2b. voice gate  Pitchedness (autocorrelation), then VoiceGate: a noise floor, the frame's motion and
 *                   its region say whether a voice is present or only steady noise. Frames without voice
 *                   stop here.
 *   3. pre-emphasis Lift the highs, which a voice loses.
 *   4. window       Hann taper.
 *   5. correlate    Autocorrelation of the frame.
 *   6. lpc          Levinson-Durbin turns the autocorrelation into all-pole filter coefficients.
 *   7. envelope     Evaluate the filter's spectrum on a grid, in dB.
 *   8. peaks        Local maxima of the envelope, refined to between grid points.
 *   9. assign       Pick F1, F2, F3 from the peaks, each within its own band.
 *  10. smooth       A Hampel filter per formant drops one-frame jumps.
 *
 * Feed frames in time order: the tracker keeps the noise floor, the smoothing history and the last
 * formants between calls. Not thread safe; wrap it (see Buffers::FormantProcessor) to share readings.
 */
class FormantTracker {
public:
    explicit FormantTracker(FormantConfig config = {});

    /**
     * @brief Run the whole chain on one frame of config.frame_size samples at config.sample_rate.
     * @param frame The samples.
     * @param hop_seconds Time since the previous frame. Zero means 512 samples.
     */
    FormantReading analyse(std::span<const double> frame, double hop_seconds = 0.0);

    /** @brief The LPC envelope in dB of the last voiced frame, 0 Hz up to half the analysis rate. Empty until one is found. */
    [[nodiscard]] const std::vector<double>& envelope() const { return m_envelope; }

    [[nodiscard]] const FormantConfig& config() const { return m_config; }

private:
    void measure(std::span<const double> frame, FormantReading& reading) const;
    [[nodiscard]] std::vector<double> decimate(std::span<const double> frame) const;
    [[nodiscard]] double periodicity(std::span<const double> signal) const;
    void pre_emphasise(std::vector<double>& signal) const;
    void assign(std::span<const double> peak_hz);

    FormantConfig m_config;
    size_t m_factor;
    double m_rate;
    std::vector<double> m_kernel;
    VoiceGate m_gate;
    std::array<HampelFilter<double>, 3> m_smoothers;
    std::array<double, 3> m_last {};
    std::vector<double> m_envelope;
};

} // namespace MayaFlux::Kinesis
