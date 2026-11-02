#include "FormantTransfer.hpp"

#include "Envelope.hpp"

#include <MayaFlux/Kinesis/Discrete/Coefficients.hpp>
#include <MayaFlux/Nodes/Network/Operators/RelationOperators.hpp>

namespace MayaFlux::Nodes::Network {

namespace D = Kinesis::Discrete;

namespace {

    /// Loudness rises fast and falls slower, so syllables keep their attack.
    constexpr double k_loudness_attack = 0.005;
    constexpr double k_loudness_release = 0.08;

    /// An envelope_follow array that starts at @p level instead of at zero.
    std::vector<double> follower_at(double level, double attack, double release, double rate)
    {
        auto coefs = D::envelope_follow_state(attack, release, rate, D::EnvelopeMode::PEAK);
        coefs[3] = std::abs(level);
        return coefs;
    }

} // namespace

FormantTransfer::FormantTransfer(std::shared_ptr<Buffers::FormantProcessor> formants,
    std::shared_ptr<Node> carrier,
    FormantTransferConfig config)
    : m_formants(std::move(formants))
    , m_controls(std::make_shared<Controls>())
    , m_scale(std::make_shared<Constant>(0.0))
    , m_relation(std::make_shared<RelationNetwork>())
{
    m_controls->glide.store(std::max(config.glide, 0.0));
    m_controls->shift.store(std::max(config.shift, 0.01));
    m_controls->sensitivity.store(std::max(config.sensitivity, 0.0));
    m_controls->unvoiced.store(std::clamp(config.unvoiced, 0.0, 1.0));

    const double rate = config.sample_rate;
    const auto& start = config.start_hz;

    m_relation->add_slots({
        { .name = "f1", .level = start[0] },
        { .name = "f2", .level = start[1] },
        { .name = "f3", .level = start[2] },
        { .name = "raw_f1", .level = start[0] },
        { .name = "raw_f2", .level = start[1] },
        { .name = "raw_f3", .level = start[2] },
        { .name = "loudness_raw" },
        { .name = "loudness" },
        { .name = "voiced", .active = false },
    });

    // Once per block: the latest reading into the raw slots, the voiced gate, and last block's loudness into the scale.
    m_relation->add<StepOperator>(
        [formants = m_formants, controls = m_controls, scale = m_scale](
            const std::shared_ptr<StepOperator>&, std::span<RelationSlot> slots, uint32_t) {
            const double shift = controls->shift.load(std::memory_order_relaxed);

            if (controls->manual.load(std::memory_order_acquire)) {
                for (size_t i = 0; i < 3; ++i)
                    slots[RAW_F1 + i].level = controls->manual_hz[i].load(std::memory_order_relaxed) * shift;
                slots[LOUDNESS_RAW].level = controls->manual_loudness.load(std::memory_order_relaxed);
                slots[VOICED].set_active(true);
                scale->set_constant(slots[LOUDNESS].level);
                return;
            }

            const auto reading = formants->latest();

            if (reading.voiced) {
                slots[RAW_F1].level = reading.f1 * shift;
                slots[RAW_F2].level = reading.f2 * shift;
                slots[RAW_F3].level = reading.f3 * shift;
            }

            const double keep = reading.voiced ? 1.0 : controls->unvoiced.load(std::memory_order_relaxed);
            const double loud = reading.rms * controls->sensitivity.load(std::memory_order_relaxed) * keep;
            slots[LOUDNESS_RAW].level = std::clamp(loud, 0.0, 1.0);

            slots[VOICED].set_active(reading.voiced);
            scale->set_constant(slots[LOUDNESS].level);
        });

    m_relation->add<AdvanceOperator>();

    const auto glide_decay = [controls = m_controls, rate] {
        return D::decay_per_sample(controls->glide.load(std::memory_order_relaxed), rate);
    };

    for (uint32_t i = 0; i < 3; ++i) {
        const double glide = m_controls->glide.load();
        auto follow = m_relation->add<DeriveOperator>(
            F1 + i, std::vector<size_t> { RAW_F1 + i }, D::envelope_follow, follower_at(start[i], glide, glide, rate));
        follow->map("c0", Source(glide_decay));
        follow->map("c1", Source(glide_decay));
    }

    m_relation->add<DeriveOperator>(
        LOUDNESS, std::vector<size_t> { LOUDNESS_RAW }, D::envelope_follow,
        follower_at(0.0, k_loudness_attack, k_loudness_release, rate));

    m_voice = std::make_shared<ResonatorNetwork>(
        std::vector<double>(start.begin(), start.end()),
        std::vector<double>(config.q.begin(), config.q.end()));
    m_voice->set_exciter(std::move(carrier));
    m_voice->map_parameter("retune", m_relation);
    m_voice->map_parameter("scale", m_scale, MappingMode::BROADCAST);
}

void FormantTransfer::set_manual(std::array<double, 3> formants_hz, double loudness)
{
    for (size_t i = 0; i < 3; ++i)
        m_controls->manual_hz[i].store(std::max(formants_hz[i], 1.0), std::memory_order_relaxed);
    m_controls->manual_loudness.store(std::clamp(loudness, 0.0, 1.0), std::memory_order_relaxed);
    m_controls->manual.store(true, std::memory_order_release);
}

double FormantTransfer::formant(size_t index) const
{
    return index < 3 ? m_relation->get_slot(static_cast<uint32_t>(F1 + index)).level : 0.0;
}

double FormantTransfer::loudness() const
{
    return m_relation->get_slot(LOUDNESS).level;
}

bool FormantTransfer::voiced() const
{
    return m_relation->get_slot(VOICED).active;
}

void FormantTransfer::on_voice(std::function<void()> callback, bool on_stop)
{
    auto& slot = m_relation->get_slot(VOICED);
    (on_stop ? slot.on_leave : slot.on_enter).push_back(std::move(callback));
}

} // namespace MayaFlux::Nodes::Network
