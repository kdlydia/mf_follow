#include "EnvelopeFollower.hpp"

namespace MayaFlux::Nodes::Filters {

namespace D = Kinesis::Discrete;

EnvelopeFollower::EnvelopeFollower(double attack, double release, D::EnvelopeMode mode)
    : m_attack(std::max(attack, 0.0))
    , m_release(std::max(release, 0.0))
    , m_mode(mode)
    , m_coefs(D::envelope_follow_state(attack, release, m_sample_rate, mode))
{
}

EnvelopeFollower::EnvelopeFollower(const std::shared_ptr<Node>& input, double attack, double release, D::EnvelopeMode mode)
    : EnvelopeFollower(attack, release, mode)
{
    m_input_node = input;
}

void EnvelopeFollower::set_times(double attack, double release)
{
    m_attack.store(std::max(attack, 0.0), std::memory_order_relaxed);
    m_release.store(std::max(release, 0.0), std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

void EnvelopeFollower::set_mode(D::EnvelopeMode mode)
{
    m_mode.store(mode, std::memory_order_relaxed);
    m_version.fetch_add(1, std::memory_order_release);
}

double EnvelopeFollower::get_db(double floor_db) const
{
    return D::amplitude_to_db(m_last_output, floor_db);
}

void EnvelopeFollower::refresh()
{
    const uint64_t version = m_version.load(std::memory_order_acquire);
    if (version == m_coef_version && m_sample_rate == m_coef_rate)
        return;
    D::envelope_set_times(m_coefs, get_attack(), get_release(), static_cast<double>(m_sample_rate));
    D::envelope_set_mode(m_coefs, get_mode());
    m_coef_version = version;
    m_coef_rate = m_sample_rate;
}

double EnvelopeFollower::process_sample(double input)
{
    refresh();

    double followed = input;
    if (m_input_node) {
        atomic_inc_modulator_count(m_input_node->m_modulator_count, 1);
        const uint32_t state = m_input_node->m_state.load();
        if (state & NodeState::PROCESSED) {
            followed += m_input_node->get_last_output();
        } else {
            followed += m_input_node->process_sample(input);
            atomic_add_flag(m_input_node->m_state, NodeState::PROCESSED);
        }
    }

    m_last_input = followed;
    const double output = D::envelope_follow(std::span<const double>(&followed, 1), m_coefs);
    m_last_output = output;

    if ((!m_state_saved || m_fire_events_during_snapshot) && !m_networked_node)
        notify_tick(output);

    if (m_input_node) {
        atomic_dec_modulator_count(m_input_node->m_modulator_count, 1);
        try_reset_processed_state(m_input_node);
    }

    return output;
}

std::vector<double> EnvelopeFollower::process_batch(unsigned int num_samples)
{
    std::vector<double> output(num_samples);
    for (unsigned int i = 0; i < num_samples; ++i)
        output[i] = process_sample(0.0);
    return output;
}

void EnvelopeFollower::save_state()
{
    m_saved_coefs = m_coefs;
    m_saved_last_output = m_last_output;

    if (m_input_node)
        m_input_node->save_state();

    m_state_saved = true;
}

void EnvelopeFollower::restore_state()
{
    m_coefs = m_saved_coefs;
    m_last_output = m_saved_last_output;

    if (m_input_node)
        m_input_node->restore_state();

    m_state_saved = false;
}

void EnvelopeFollower::update_context(double value)
{
    m_context.value = value;
    m_context.input = m_last_input;
    m_context.db = D::amplitude_to_db(value);
}

NodeContext& EnvelopeFollower::get_last_context()
{
    return m_context;
}

void EnvelopeFollower::notify_tick(double value)
{
    update_context(value);
    auto& ctx = get_last_context();
    m_callbacks.fire(ctx);
    m_conditional_callbacks.fire(ctx);
}

HookId EnvelopeFollower::on_tick(const TypedHook<EnvelopeContext>& callback)
{
    return m_callbacks.add(adapt_hook(callback), callback.target_type());
}

HookId EnvelopeFollower::on_tick_if(const NodeCondition& condition, const TypedHook<EnvelopeContext>& callback)
{
    return m_conditional_callbacks.add_conditional(adapt_hook(callback), condition, callback.target_type());
}

std::vector<std::pair<ModulatorRole, std::shared_ptr<Node>>> EnvelopeFollower::get_modulators() const
{
    if (m_input_node)
        return { { ModulatorRole::SignalMod, m_input_node } };
    return {};
}

} // namespace MayaFlux::Nodes::Filters
