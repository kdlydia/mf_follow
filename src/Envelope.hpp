#pragma once

namespace MayaFlux::Kinesis::Discrete {

/**
 * @enum EnvelopeMode
 * @brief What the envelope follower measures.
 */
enum class EnvelopeMode : uint8_t {
    PEAK, ///< Follows |x|. Fast, tracks transients.
    RMS, ///< Follows x^2 and returns its root. Tracks loudness; a sine of amplitude A settles at A / sqrt(2).
};

/**
 * @brief Builds the array envelope_follow reads and writes.
 * @param attack Seconds for a rise to cover 1 - 1/e of a step
 * @param release Seconds for a fall to cover 1 - 1/e of a step
 * @param sample_rate Sample rate in Hz
 * @param mode PEAK or RMS
 *
 * Layout: [attack_decay, release_decay, mode, level]. The decays are
 * decay_per_sample of each time. level is the follower's memory: |x| in
 * PEAK mode, x^2 in RMS mode. A time of zero follows instantly.
 */
[[nodiscard]] std::vector<double> envelope_follow_state(
    double attack, double release, double sample_rate, EnvelopeMode mode = EnvelopeMode::PEAK);

/**
 * @brief Changes the attack and release of an existing array, keeping its level.
 *
 * Negative times are treated as zero. An array shorter than four does nothing.
 */
void envelope_set_times(
    std::span<double> coefs, double attack, double release, double sample_rate) noexcept;

/**
 * @brief Changes the mode of an existing array.
 *
 * The level is converted so the output does not jump: squared going to RMS,
 * rooted going to PEAK.
 */
void envelope_set_mode(std::span<double> coefs, EnvelopeMode mode) noexcept;

/**
 * @brief The envelope the array currently holds, without advancing it.
 */
[[nodiscard]] double envelope_value(std::span<const double> coefs) noexcept;

/**
 * @brief One step of an attack/release envelope follower.
 *
 * coefs is the array envelope_follow_state builds. Reads window[0], moves the
 * level toward |x| (PEAK) or x^2 (RMS) with the attack decay when rising and
 * the release decay when falling, and returns the envelope. This is
 * Kinesis::damp_asymmetric with the exponentials computed once instead of per
 * call. An empty window steps toward zero; an array shorter than four gives zero.
 */
[[nodiscard]] double envelope_follow(
    std::span<const double> window, std::span<double> coefs) noexcept;

/**
 * @brief envelope_follow over a block.
 * @param in Samples to follow
 * @param out Envelope per sample. May alias @p in. Left untouched when
 *        smaller than @p in, which runs the follower for its final value only.
 * @param coefs The array envelope_follow_state builds
 * @return The envelope after the last sample
 */
double envelope_block(
    std::span<const double> in, std::span<double> out, std::span<double> coefs) noexcept;

/**
 * @brief Amplitude to decibels, 20 * log10(|amplitude|), floored.
 * @param amplitude Linear amplitude
 * @param floor_db Returned for silence and anything quieter
 */
[[nodiscard]] double amplitude_to_db(double amplitude, double floor_db = -120.0) noexcept;

/**
 * @brief Decibels to amplitude, 10^(db / 20).
 */
[[nodiscard]] double db_to_amplitude(double db) noexcept;

} // namespace MayaFlux::Kinesis::Discrete
