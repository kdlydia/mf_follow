#pragma once

#include "Envelope.hpp"

#include <MayaFlux/Nodes/Node.hpp>

namespace MayaFlux::Nodes::Filters {

/**
 * @class EnvelopeContext
 * @brief What an EnvelopeFollower's tick hooks see.
 */
class EnvelopeContext : public NodeContext {
public:
    EnvelopeContext(double value, double input, double db)
        : NodeContext(value)
        , input(input)
        , db(db)
    {
    }

    double input; ///< The sample that was followed
    double db; ///< value in decibels
};

/**
 * @class EnvelopeFollower
 * @brief Attack/release envelope follower, one sample at a time.
 *
 * Follows the sample it is given, plus the output of an input node when one
 * is set, the same way IIR and FIR take their input. Works as a chain link
 * (source >> follower) or standalone with set_input_node. The output is the
 * envelope, never negative.
 *
 * Runs Kinesis::Discrete::envelope_follow, so it produces exactly what
 * Buffers::EnvelopeProcessor produces on the same samples. Attack, release
 * and mode can be set from any thread and take effect at the next sample.
 */
class EnvelopeFollower : public Node {
public:
    /**
     * @param attack Seconds; see Kinesis::Discrete::envelope_follow_state
     * @param release Seconds
     * @param mode PEAK or RMS
     */
    explicit EnvelopeFollower(
        double attack = 0.01,
        double release = 0.1,
        Kinesis::Discrete::EnvelopeMode mode = Kinesis::Discrete::EnvelopeMode::PEAK);

    EnvelopeFollower(
        const std::shared_ptr<Node>& input,
        double attack = 0.01,
        double release = 0.1,
        Kinesis::Discrete::EnvelopeMode mode = Kinesis::Discrete::EnvelopeMode::PEAK);

    ~EnvelopeFollower() override = default;

    double process_sample(double input = 0.) override;
    std::vector<double> process_batch(unsigned int num_samples) override;

    void set_input_node(const std::shared_ptr<Node>& input_node) { m_input_node = input_node; }
    [[nodiscard]] std::shared_ptr<Node> get_input_node() const { return m_input_node; }

    void set_times(double attack, double release);
    void set_attack(double attack) { set_times(attack, get_release()); }
    void set_release(double release) { set_times(get_attack(), release); }
    void set_mode(Kinesis::Discrete::EnvelopeMode mode);

    [[nodiscard]] double get_attack() const { return m_attack.load(std::memory_order_relaxed); }
    [[nodiscard]] double get_release() const { return m_release.load(std::memory_order_relaxed); }
    [[nodiscard]] Kinesis::Discrete::EnvelopeMode get_mode() const { return m_mode.load(std::memory_order_relaxed); }

    /**
     * @brief The last output in decibels.
     */
    [[nodiscard]] double get_db(double floor_db = -120.0) const;

    void save_state() override;
    void restore_state() override;

    NodeContext& get_last_context() override;

    HookId on_tick(const TypedHook<EnvelopeContext>& callback);
    HookId on_tick_if(const NodeCondition& condition, const TypedHook<EnvelopeContext>& callback);

    [[nodiscard]] std::vector<std::pair<ModulatorRole, std::shared_ptr<Node>>> get_modulators() const override;

protected:
    void update_context(double value) override;
    void notify_tick(double value) override;

private:
    std::shared_ptr<Node> m_input_node;

    std::atomic<double> m_attack;
    std::atomic<double> m_release;
    std::atomic<Kinesis::Discrete::EnvelopeMode> m_mode;
    std::atomic<uint64_t> m_version { 1 };

    std::vector<double> m_coefs;
    std::vector<double> m_saved_coefs;
    uint64_t m_coef_version { 0 };
    uint32_t m_coef_rate { 0 };

    double m_last_input { 0.0 };
    double m_saved_last_output { 0.0 };

    EnvelopeContext m_context { 0.0, 0.0, -120.0 };

    void refresh();
};

} // namespace MayaFlux::Nodes::Filters
