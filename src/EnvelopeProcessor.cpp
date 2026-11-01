#include "EnvelopeProcessor.hpp"

#include <MayaFlux/API/Config.hpp>
#include <MayaFlux/API/Graph.hpp>

namespace MayaFlux::Buffers {

namespace D = Kinesis::Discrete;

EnvelopeProcessor::EnvelopeProcessor(double attack, double release, D::EnvelopeMode mode, EnvelopeOutput output)
    : m_attack(std::max(attack, 0.0))
    , m_release(std::max(release, 0.0))
    , m_mode(mode)
    , m_output(output)
{
}

std::shared_ptr<EnvelopeProcessor> EnvelopeProcessor::listen(
    uint32_t channel, double attack, double release, D::EnvelopeMode mode)
{
    auto input = create_input_listener_buffer(channel, false);
    auto processor = create_processor<EnvelopeProcessor>(input, attack, release, mode, EnvelopeOutput::FEATURE);
    processor->set_skip_silent_blocks(true);
    return processor;
}

bool EnvelopeProcessor::is_compatible_with(const std::shared_ptr<Buffer>& buffer) const
{
    return std::dynamic_pointer_cast<AudioBuffer>(buffer) != nullptr;
}

void EnvelopeProcessor::on_attach(const std::shared_ptr<Buffer>& buffer)
{
    m_sample_rate = static_cast<double>(Config::get_sample_rate());
    state_for(buffer.get());
}

void EnvelopeProcessor::on_detach(const std::shared_ptr<Buffer>& buffer)
{
    std::lock_guard lock(m_states_mutex);
    m_states.erase(buffer.get());
}

void EnvelopeProcessor::set_times(double attack, double release)
{
    m_attack.store(std::max(attack, 0.0), std::memory_order_relaxed);
    m_release.store(std::max(release, 0.0), std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

void EnvelopeProcessor::set_mode(D::EnvelopeMode mode)
{
    m_mode.store(mode, std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

void EnvelopeProcessor::set_sidechain(const std::shared_ptr<AudioBuffer>& source)
{
    m_sidechain.store(source);
}

void EnvelopeProcessor::clear_sidechain()
{
    m_sidechain.store(std::weak_ptr<AudioBuffer> {});
}

double EnvelopeProcessor::feature(const std::shared_ptr<Buffer>& buffer) const
{
    std::lock_guard lock(m_states_mutex);
    const auto found = m_states.find(buffer.get());
    return found == m_states.end() ? 0.0 : found->second->feature.load(std::memory_order_relaxed);
}

double EnvelopeProcessor::latest_db(double floor_db) const
{
    return D::amplitude_to_db(latest(), floor_db);
}

EnvelopeProcessor::State& EnvelopeProcessor::state_for(const Buffer* buffer)
{
    if (const auto found = m_states.find(buffer); found != m_states.end())
        return *found->second;

    auto state = std::make_unique<State>();
    state->coefs = D::envelope_follow_state(get_attack(), get_release(), m_sample_rate, get_mode());
    state->version = m_version.load(std::memory_order_acquire);

    std::lock_guard lock(m_states_mutex);
    return *m_states.emplace(buffer, std::move(state)).first->second;
}

void EnvelopeProcessor::refresh(State& state) const
{
    const uint64_t version = m_version.load(std::memory_order_acquire);
    if (state.version == version)
        return;
    D::envelope_set_times(state.coefs, get_attack(), get_release(), m_sample_rate);
    D::envelope_set_mode(state.coefs, get_mode());
    state.version = version;
}

void EnvelopeProcessor::processing_function(const std::shared_ptr<Buffer>& buffer)
{
    auto audio = std::dynamic_pointer_cast<AudioBuffer>(buffer);
    if (!audio)
        return;

    auto& data = audio->get_data();
    if (data.empty())
        return;

    const auto sidechain = m_sidechain.load().lock();
    const std::span<const double> source = sidechain
        ? std::span<const double>(sidechain->get_data())
        : std::span<const double>(data);

    if (m_skip_silent.load(std::memory_order_relaxed)
        && std::ranges::all_of(source, [](double v) { return v == 0.0; }))
        return;

    State& state = state_for(buffer.get());
    refresh(state);

    const EnvelopeOutput output = get_output();
    double env = 0.0;

    if (output == EnvelopeOutput::FEATURE) {
        env = D::envelope_block(source, {}, state.coefs);
    } else {
        // A sidechain shorter than the buffer leaves the tail at the last envelope value.
        const size_t n = std::min(source.size(), data.size());
        state.envelope.resize(data.size());
        env = D::envelope_block(source.first(n), std::span(state.envelope).first(n), state.coefs);
        std::fill(state.envelope.begin() + static_cast<std::ptrdiff_t>(n), state.envelope.end(), env);

        if (output == EnvelopeOutput::SIGNAL) {
            std::ranges::copy(state.envelope, data.begin());
        } else if (m_gain_law) {
            for (size_t i = 0; i < data.size(); ++i)
                data[i] *= m_gain_law(state.envelope[i]);
        } else {
            for (size_t i = 0; i < data.size(); ++i)
                data[i] *= state.envelope[i];
        }
    }

    state.feature.store(env, std::memory_order_relaxed);
    m_latest.store(env, std::memory_order_relaxed);

    if (m_observer)
        m_observer(env);
}

} // namespace MayaFlux::Buffers
