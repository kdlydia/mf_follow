#include "GlottalSource.hpp"

namespace MayaFlux::Nodes::Generator {

GlottalSource::~GlottalSource() = default;

GlottalSource::GlottalSource(float frequency, double tilt, double amplitude)
    : m_tilt(std::clamp(tilt, 0.0, 0.9999))
{
    m_amplitude = amplitude;
    m_frequency = frequency;
}

void GlottalSource::set_tilt(double tilt)
{
    m_tilt = std::clamp(tilt, 0.0, 0.9999);
}

double GlottalSource::process_sample(double /*input*/)
{
    const auto rate = static_cast<double>(m_timing_rate);

    bool pulse = false;
    if (rate > 0.0) {
        m_phase += static_cast<double>(m_frequency) / rate;
        if (m_phase >= 1.0) {
            m_phase -= std::floor(m_phase);
            pulse = true;
        }
    }

    m_glottal = (pulse ? 1.0 : 0.0) + m_tilt * m_glottal;
    const double out = m_amplitude * m_glottal;

    m_last_output = out;

    if ((!m_state_saved || m_fire_events_during_snapshot) && !m_networked_node)
        notify_tick(out);

    return out;
}

std::vector<double> GlottalSource::process_batch(unsigned int num_samples)
{
    std::vector<double> out(num_samples);
    for (unsigned int i = 0; i < num_samples; ++i)
        out[i] = process_sample(0.0);
    return out;
}

void GlottalSource::save_state()
{
    m_saved_phase = m_phase;
    m_saved_glottal = m_glottal;
    m_saved_last_output = m_last_output;
    m_state_saved = true;
}

void GlottalSource::restore_state()
{
    m_phase = m_saved_phase;
    m_glottal = m_saved_glottal;
    m_last_output = m_saved_last_output;
    m_state_saved = false;
}

} // namespace MayaFlux::Nodes::Generator
