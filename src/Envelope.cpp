#include "Envelope.hpp"

#include <MayaFlux/Kinesis/Discrete/Coefficients.hpp>

namespace MayaFlux::Kinesis::Discrete {

namespace {

    constexpr size_t k_attack = 0;
    constexpr size_t k_release = 1;
    constexpr size_t k_mode = 2;
    constexpr size_t k_level = 3;
    constexpr size_t k_size = 4;

    bool is_rms(std::span<const double> coefs) noexcept
    {
        return coefs[k_mode] > 0.5;
    }

    double decay(double seconds, double sample_rate) noexcept
    {
        if (seconds <= 0.0 || sample_rate <= 0.0)
            return 0.0;
        return decay_per_sample(seconds, sample_rate);
    }

    double step(double x, std::span<double> coefs) noexcept
    {
        const bool rms = is_rms(coefs);
        const double target = rms ? x * x : std::abs(x);
        double& level = coefs[k_level];
        const double g = target > level ? coefs[k_attack] : coefs[k_release];
        level = target + g * (level - target);
        return rms ? std::sqrt(level) : level;
    }

} // namespace

std::vector<double> envelope_follow_state(
    double attack, double release, double sample_rate, EnvelopeMode mode)
{
    std::vector<double> coefs(k_size, 0.0);
    envelope_set_times(coefs, attack, release, sample_rate);
    coefs[k_mode] = mode == EnvelopeMode::RMS ? 1.0 : 0.0;
    return coefs;
}

void envelope_set_times(
    std::span<double> coefs, double attack, double release, double sample_rate) noexcept
{
    if (coefs.size() < k_size)
        return;
    coefs[k_attack] = decay(attack, sample_rate);
    coefs[k_release] = decay(release, sample_rate);
}

void envelope_set_mode(std::span<double> coefs, EnvelopeMode mode) noexcept
{
    if (coefs.size() < k_size)
        return;
    const bool to_rms = mode == EnvelopeMode::RMS;
    if (to_rms != is_rms(coefs)) {
        const double level = coefs[k_level];
        coefs[k_level] = to_rms ? level * level : std::sqrt(level);
    }
    coefs[k_mode] = to_rms ? 1.0 : 0.0;
}

double envelope_value(std::span<const double> coefs) noexcept
{
    if (coefs.size() < k_size)
        return 0.0;
    return is_rms(coefs) ? std::sqrt(coefs[k_level]) : coefs[k_level];
}

double envelope_follow(std::span<const double> window, std::span<double> coefs) noexcept
{
    if (coefs.size() < k_size)
        return 0.0;
    return step(window.empty() ? 0.0 : window.front(), coefs);
}

double envelope_block(
    std::span<const double> in, std::span<double> out, std::span<double> coefs) noexcept
{
    if (coefs.size() < k_size)
        return 0.0;

    const bool write = out.size() >= in.size();
    for (size_t i = 0; i < in.size(); ++i) {
        const double env = step(in[i], coefs);
        if (write)
            out[i] = env;
    }
    return envelope_value(coefs);
}

double amplitude_to_db(double amplitude, double floor_db) noexcept
{
    const double a = std::abs(amplitude);
    if (a <= 0.0)
        return floor_db;
    return std::max(20.0 * std::log10(a), floor_db);
}

double db_to_amplitude(double db) noexcept
{
    return std::pow(10.0, db / 20.0);
}

} // namespace MayaFlux::Kinesis::Discrete
