#include "EnvelopeProcessor.hpp"

#include <MayaFlux/API/Graph.hpp>
#include <MayaFlux/Buffers/BufferProcessingChain.hpp>
#include <MayaFlux/Buffers/BufferSpec.hpp>

namespace MayaFlux::Buffers {

namespace D = Kinesis::Discrete;

namespace {

    double current_sample_rate()
    {
        return static_cast<double>(s_registered_sample_rate);
    }

} // namespace

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
    auto input = create_input_listener_buffer(channel, true);
    auto processor = std::make_shared<EnvelopeProcessor>(attack, release, mode, EnvelopeOutput::FEATURE);
    processor->set_skip_silent_blocks(true);
    processor->set_silence_after(true);
    input->get_processing_chain()->add_processor(processor, input);
    return processor;
}

bool EnvelopeProcessor::is_compatible_with(const std::shared_ptr<Buffer>& buffer) const
{
    return std::dynamic_pointer_cast<AudioBuffer>(buffer) != nullptr;
}

void EnvelopeProcessor::on_attach(const std::shared_ptr<Buffer>& buffer)
{
    (void)state_for(buffer);
}

void EnvelopeProcessor::on_detach(const std::shared_ptr<Buffer>& buffer)
{
    m_states.update([key = buffer.get()](States& states) { return states.erase(key); });
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
    m_sidechain.publish(std::weak_ptr<AudioBuffer>(source));
}

void EnvelopeProcessor::clear_sidechain()
{
    m_sidechain.reset();
}

double EnvelopeProcessor::feature(const std::shared_ptr<Buffer>& buffer) const
{
    const auto state = find_state(buffer);
    return state ? state->feature.load(std::memory_order_relaxed) : 0.0;
}

double EnvelopeProcessor::latest_db(double floor_db) const
{
    return D::amplitude_to_db(latest(), floor_db);
}

std::shared_ptr<EnvelopeProcessor::State> EnvelopeProcessor::find_state(const std::shared_ptr<Buffer>& buffer) const
{
    if (const auto states = m_states.read()) {
        if (const auto found = states->find(buffer.get()); found != states->end() && found->second->belongs_to(buffer))
            return found->second;
    }
    return nullptr;
}

std::shared_ptr<EnvelopeProcessor::State> EnvelopeProcessor::state_for(const std::shared_ptr<Buffer>& buffer)
{
    if (auto state = find_state(buffer))
        return state;

    auto state = std::make_shared<State>();
    state->owner = buffer;
    state->sample_rate = current_sample_rate();
    state->coefs = D::envelope_follow_state(get_attack(), get_release(), state->sample_rate, get_mode());
    state->version = m_version.load(std::memory_order_acquire);

    // A stale entry for a buffer that died at the same address is replaced here.
    m_states.update([key = buffer.get(), state](States& states) { states.insert_or_assign(key, state); });
    return state;
}

void EnvelopeProcessor::refresh(State& state) const
{
    const uint64_t version = m_version.load(std::memory_order_acquire);
    const double rate = current_sample_rate();
    if (state.version == version && state.sample_rate == rate)
        return;
    D::envelope_set_times(state.coefs, get_attack(), get_release(), rate);
    D::envelope_set_mode(state.coefs, get_mode());
    state.version = version;
    state.sample_rate = rate;
}

void EnvelopeProcessor::processing_function(const std::shared_ptr<Buffer>& buffer)
{
    auto audio = std::dynamic_pointer_cast<AudioBuffer>(buffer);
    if (!audio)
        return;

    auto& data = audio->get_data();
    if (data.empty())
        return;

    std::shared_ptr<AudioBuffer> sidechain;
    if (const auto held = m_sidechain.read())
        sidechain = held->lock();

    const std::span<const double> source = sidechain
        ? std::span<const double>(sidechain->get_data())
        : std::span<const double>(data);

    if (m_skip_silent.load(std::memory_order_relaxed)
        && std::ranges::all_of(source, [](double v) { return v == 0.0; }))
        return;

    const auto state = state_for(buffer);
    refresh(*state);

    const EnvelopeOutput output = get_output();
    double env = 0.0;

    if (output == EnvelopeOutput::FEATURE) {
        env = D::envelope_block(source, {}, state->coefs);
        if (m_silence_after.load(std::memory_order_relaxed))
            std::ranges::fill(data, 0.0);
    } else {
        // A sidechain shorter than the buffer leaves the tail at the last envelope value.
        const size_t n = std::min(source.size(), data.size());
        state->envelope.resize(data.size());
        env = D::envelope_block(source.first(n), std::span(state->envelope).first(n), state->coefs);
        std::fill(state->envelope.begin() + static_cast<std::ptrdiff_t>(n), state->envelope.end(), env);

        if (output == EnvelopeOutput::SIGNAL) {
            std::ranges::copy(state->envelope, data.begin());
        } else if (m_gain_law) {
            for (size_t i = 0; i < data.size(); ++i)
                data[i] *= m_gain_law(state->envelope[i]);
        } else {
            for (size_t i = 0; i < data.size(); ++i)
                data[i] *= state->envelope[i];
        }
    }

    state->feature.store(env, std::memory_order_relaxed);
    m_latest.store(env, std::memory_order_relaxed);

    if (m_observer)
        m_observer(env);
}

} // namespace MayaFlux::Buffers
