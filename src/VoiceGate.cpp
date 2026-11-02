#include "VoiceGate.hpp"

namespace MayaFlux::Kinesis {

namespace {

    /** @brief Level scale: -80 dB maps to 0 and -20 dB to 1. */
    constexpr double k_low_db = -80.0;
    constexpr double k_span_db = 60.0;

    /** @brief The voice cell of the 2 by 2 lattice: above the floor, and pitched. */
    constexpr glm::uvec2 k_voice_cell { 1U, 1U };

}

VoiceGate::VoiceGate(VoiceGateConfig config)
    : m_config(config)
    , m_floor(Stochastic::EstimateModel::QUIET_PERIOD_FLOOR, 0.05, config.floor_window)
    , m_motion(Stochastic::EstimateModel::ROLLING_VARIANCE, config.floor_window, 0.05, 8)
{
    m_motion.filter_order(1, 8, 3.5);
    m_motion.enable_trajectory_2d(Lattice2D::ndc_quadrants(), config.vote_window);
}

VoiceActivity VoiceGate::update(double rms, double periodicity, double dt)
{
    VoiceActivity activity;
    activity.periodicity = periodicity;
    activity.level = std::clamp((20.0 * std::log10(rms + 1e-9) - k_low_db) / k_span_db, 0.0, 1.0);

    // The floor learns what the quiet stretches sound like, so a fan that was already running becomes the floor.
    activity.floor = m_floor.update(activity.level);

    // The point on the plane. Its signs are the lattice's quadrants: x above the floor, y pitched.
    const glm::vec2 point(
        static_cast<float>(activity.level - activity.floor - m_config.margin),
        static_cast<float>(periodicity - m_config.min_periodicity));
    m_motion.update(point, dt);

    const glm::vec2 velocity = m_motion.derivative<1>();
    activity.speed = glm::length(velocity);
    activity.onset = velocity.x > m_config.onset_rate;

    activity.warming = ++m_frames < m_config.warm_frames;
    if (!activity.warming) {
        const auto [cell, votes] = m_motion.trajectory_2d()->dominant_cell(m_config.vote_window);
        activity.voice = cell == k_voice_cell && votes * 2 > m_config.vote_window;
    }
    return activity;
}

} // namespace MayaFlux::Kinesis
