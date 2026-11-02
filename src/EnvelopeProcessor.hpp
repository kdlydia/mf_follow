#pragma once

#include "Envelope.hpp"

#include <MayaFlux/Buffers/AudioBuffer.hpp>
#include <MayaFlux/Buffers/BufferProcessor.hpp>
#include <MayaFlux/Transitive/Memory/FlatMap.hpp>
#include <MayaFlux/Transitive/Memory/SnapshotCell.hpp>

namespace MayaFlux::Buffers {

/**
 * @enum EnvelopeOutput
 * @brief What an EnvelopeProcessor does with the envelope it measures.
 */
enum class EnvelopeOutput : uint8_t {
    FEATURE, ///< Leave the buffer untouched; publish the envelope at the end of each block.
    SIGNAL, ///< Overwrite the buffer with its envelope, sample by sample.
    GAIN, ///< Multiply the buffer by the envelope, or by gain_law(envelope) when one is set.
};

/**
 * @class EnvelopeProcessor
 * @brief Attack/release envelope follower that works on whole audio blocks.
 *
 * Runs Kinesis::Discrete::envelope_block over each block it is given. One
 * instance can be attached to several buffers; each keeps its own level.
 *
 * The envelope is measured on the buffer itself or, with set_sidechain(), on
 * another AudioBuffer. The sidechain is read as it stands when this processor
 * runs, so it carries this cycle's data if it was processed earlier in the
 * cycle and last cycle's otherwise. With a sidechain and EnvelopeOutput::GAIN
 * this is the base of ducking, gating and compression: pass a gain law that
 * maps the envelope to a gain.
 *
 * Parameters can be set from any thread and take effect at the next block.
 * feature() and latest() can be read from any thread. The observer runs on
 * the processing thread. Per-buffer state and the sidechain are held in
 * Memory::SnapshotCell, so neither the processing thread nor a reader ever
 * takes a lock.
 */
class EnvelopeProcessor : public BufferProcessor {
public:
    using GainLaw = std::function<double(double)>;
    using Observer = std::function<void(double)>;

    /**
     * @param attack Seconds; see Kinesis::Discrete::envelope_follow_state
     * @param release Seconds
     * @param mode PEAK or RMS
     * @param output What to do with the envelope
     */
    explicit EnvelopeProcessor(
        double attack = 0.01,
        double release = 0.1,
        Kinesis::Discrete::EnvelopeMode mode = Kinesis::Discrete::EnvelopeMode::PEAK,
        EnvelopeOutput output = EnvelopeOutput::FEATURE);

    /**
     * @brief Follows an audio input channel.
     *
     * An input listener's processing chain only runs while the buffer sits on
     * an output channel, so this registers one there, attaches a FEATURE
     * processor to the buffer's own chain, and has it silence each block after
     * measuring it so the microphone does not reach the speakers. Silent-block
     * skipping is turned on as well (see set_skip_silent_blocks).
     */
    static std::shared_ptr<EnvelopeProcessor> listen(
        uint32_t channel = 0,
        double attack = 0.01,
        double release = 0.1,
        Kinesis::Discrete::EnvelopeMode mode = Kinesis::Discrete::EnvelopeMode::PEAK);

    void processing_function(const std::shared_ptr<Buffer>& buffer) override;
    void on_attach(const std::shared_ptr<Buffer>& buffer) override;
    void on_detach(const std::shared_ptr<Buffer>& buffer) override;
    [[nodiscard]] bool is_compatible_with(const std::shared_ptr<Buffer>& buffer) const override;

    void set_times(double attack, double release);
    void set_attack(double attack) { set_times(attack, get_release()); }
    void set_release(double release) { set_times(get_attack(), release); }
    void set_mode(Kinesis::Discrete::EnvelopeMode mode);
    void set_output(EnvelopeOutput output) { m_output.store(output, std::memory_order_relaxed); }

    /**
     * @brief Function from envelope to gain, used by EnvelopeOutput::GAIN.
     *
     * Set it before attaching or while the processor is not running; it is
     * not swapped atomically. Without one the envelope is the gain.
     */
    void set_gain_law(GainLaw law) { m_gain_law = std::move(law); }

    /**
     * @brief Measure the envelope on @p source instead of the processed buffer.
     */
    void set_sidechain(const std::shared_ptr<AudioBuffer>& source);
    void clear_sidechain();

    /**
     * @brief Skip blocks that are entirely zero instead of releasing on them.
     *
     * The engine runs an input listener about twice per microphone delivery,
     * so every other input block is empty. Following those would halve the
     * level. Leave off for ordinary buffers, where silence is real.
     */
    void set_skip_silent_blocks(bool skip) { m_skip_silent.store(skip, std::memory_order_relaxed); }

    /**
     * @brief Zero the buffer after measuring it, in EnvelopeOutput::FEATURE.
     *
     * For a buffer that has to sit on an output channel only to be processed,
     * such as an input listener, so it measures without being heard.
     */
    void set_silence_after(bool silence) { m_silence_after.store(silence, std::memory_order_relaxed); }

    /**
     * @brief Called with the envelope at the end of each block, on the processing thread.
     *
     * Same threading rule as set_gain_law.
     */
    void set_observer(Observer observer) { m_observer = std::move(observer); }

    [[nodiscard]] double get_attack() const { return m_attack.load(std::memory_order_relaxed); }
    [[nodiscard]] double get_release() const { return m_release.load(std::memory_order_relaxed); }
    [[nodiscard]] Kinesis::Discrete::EnvelopeMode get_mode() const { return m_mode.load(std::memory_order_relaxed); }
    [[nodiscard]] EnvelopeOutput get_output() const { return m_output.load(std::memory_order_relaxed); }

    /**
     * @brief Envelope at the end of the last block processed for @p buffer, zero if unknown.
     */
    [[nodiscard]] double feature(const std::shared_ptr<Buffer>& buffer) const;

    /**
     * @brief Envelope at the end of the last block processed for any buffer.
     */
    [[nodiscard]] double latest() const { return m_latest.load(std::memory_order_relaxed); }

    /**
     * @brief latest() in decibels.
     */
    [[nodiscard]] double latest_db(double floor_db = -120.0) const;

private:
    struct State {
        std::weak_ptr<Buffer> owner;
        std::vector<double> coefs;
        std::vector<double> envelope;
        uint64_t version { 0 };
        double sample_rate { 0.0 };
        std::atomic<double> feature { 0.0 };

        [[nodiscard]] bool belongs_to(const std::shared_ptr<Buffer>& buffer) const
        {
            return !owner.owner_before(buffer) && !buffer.owner_before(owner);
        }
    };

    /// Keyed by address for lookup; State::owner confirms the entry still belongs to that buffer.
    using States = Memory::FlatMap<const Buffer*, std::shared_ptr<State>>;

    std::atomic<double> m_attack;
    std::atomic<double> m_release;
    std::atomic<Kinesis::Discrete::EnvelopeMode> m_mode;
    std::atomic<EnvelopeOutput> m_output;
    std::atomic<bool> m_skip_silent { false };
    std::atomic<bool> m_silence_after { false };
    std::atomic<uint64_t> m_version { 1 };
    std::atomic<double> m_latest { 0.0 };

    Memory::SnapshotCell<States> m_states;
    Memory::SnapshotCell<std::weak_ptr<AudioBuffer>> m_sidechain;

    GainLaw m_gain_law;
    Observer m_observer;

    [[nodiscard]] std::shared_ptr<State> find_state(const std::shared_ptr<Buffer>& buffer) const;
    std::shared_ptr<State> state_for(const std::shared_ptr<Buffer>& buffer);
    void refresh(State& state) const;
};

} // namespace MayaFlux::Buffers
