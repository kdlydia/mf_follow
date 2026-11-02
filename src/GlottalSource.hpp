#pragma once

#include <MayaFlux/Nodes/Generators/Generator.hpp>

namespace MayaFlux::Nodes::Generator {

/**
 * @class GlottalSource
 * @brief A pulse train with the spectral tilt of a voice: the sound the vocal folds make before the
 *        mouth shapes it.
 *
 * Emits one pulse per period at m_frequency, then runs the pulses through a one-pole integrator:
 *
 *   g[n] = pulse[n] + tilt * g[n-1]
 *   y[n] = amplitude * g[n]
 *
 * The integrator makes the harmonics fall at about 6 dB per octave above a corner near
 * (1 - tilt) * rate / (2 pi), which is how the output of a real glottis falls once the radiation from the
 * lips is counted. Each pulse jumps to the amplitude and then decays, so the peak is the amplitude as long
 * as the period is long compared with the decay. The energy grows with the tilt, though: a tilt of 0.97
 * carries far more power than the flat train at the same peak.
 *
 * Feed it into resonators tuned to formants and it sounds as a vowel. With tilt at zero it is a plain
 * unit pulse train.
 *
 * m_frequency is the pitch in Hz, so set_frequency() drives it like any other generator and a
 * modulator chain can sweep it.
 */
class GlottalSource : public Generator {
public:
    /**
     * @param frequency Pitch in Hz.
     * @param tilt Integrator pole in [0, 1). Zero is flat, about 0.97 is a voice at 48 kHz.
     * @param amplitude Peak scaling.
     */
    explicit GlottalSource(float frequency = 110.0F, double tilt = 0.97, double amplitude = 1.0);

    ~GlottalSource() override;

    /**
     * @brief Generate one sample of the tilted pulse train.
     * @param input Unused; present for node graph compatibility.
     */
    double process_sample(double input = 0.0) override;

    /**
     * @brief Generate a block of samples.
     * @param num_samples Number of samples to generate.
     */
    std::vector<double> process_batch(unsigned int num_samples) override;

    /**
     * @brief Set the integrator pole.
     * @param tilt Clamped to [0, 0.9999].
     */
    void set_tilt(double tilt);

    [[nodiscard]] double get_tilt() const { return m_tilt; }

    void save_state() override;
    void restore_state() override;

    [[nodiscard]] std::vector<std::pair<ModulatorRole, std::shared_ptr<Node>>>
    get_modulators() const override { return { }; }

private:
    double m_tilt;
    double m_glottal { 0.0 };

    double m_saved_phase { 0.0 };
    double m_saved_glottal { 0.0 };
    double m_saved_last_output { 0.0 };
};

} // namespace MayaFlux::Nodes::Generator
