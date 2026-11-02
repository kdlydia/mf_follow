#pragma once

#include "FormantTracker.hpp"

#include <MayaFlux/Buffers/AudioBuffer.hpp>
#include <MayaFlux/Buffers/BufferManager.hpp>
#include <MayaFlux/Buffers/BufferProcessor.hpp>
#include <MayaFlux/Transitive/Memory/SeqLock.hpp>

namespace MayaFlux::Buffers {

/**
 * @class FormantProcessor
 * @brief A BufferProcessor that follows the formants of whatever audio passes through its buffer.
 *
 * Each time the buffer is processed the block goes into a rolling frame of config.frame_size samples
 * and a Kinesis::FormantTracker analyses it. The reading, the last frame and the last envelope can be
 * read from any thread while the processing thread keeps writing them: they sit behind a
 * Memory::Seqlock, so the writer never blocks or allocates and readers retry on a write in progress.
 *
 * Empty cycles are skipped. The engine can run a buffer more often than a microphone delivers into it,
 * and a cycle with no new input is all zeros. Pushing that into the frame would halve the energy and
 * wreck the autocorrelation, so such blocks leave the frame as it is.
 *
 * By default the buffer is cleared after it has been read, so a listening buffer that sits on an output
 * channel cannot send the microphone to the speakers. Pass silence_buffer = false to analyse audio that
 * should keep playing.
 *
 * @code
 * // Follow the microphone, on the caller's buffer manager
 * auto formants = Buffers::FormantProcessor::listen(manager);
 * auto reading = formants->latest();
 *
 * // Follow any audio buffer without silencing it
 * auto tap = std::make_shared<Buffers::FormantProcessor>(Kinesis::FormantConfig {}, false);
 * buffer->get_processing_chain()->add_processor(tap, buffer);
 * @endcode
 */
class FormantProcessor : public BufferProcessor {
public:
    /**
     * @param config Tracker settings. config.sample_rate must match the audio.
     * @param silence_buffer Clear the buffer after analysis.
     */
    explicit FormantProcessor(Kinesis::FormantConfig config = {}, bool silence_buffer = true);

    /**
     * @brief Follow an input channel of @p manager.
     *
     * An input listener's processing chain only runs while the buffer sits on an output channel, so
     * this adds one there, registers it as a listener on the same input channel and attaches a
     * FormantProcessor to the buffer's own chain. The processor clears the buffer after reading it.
     * Enable the input in the stream settings first.
     *
     * @param manager Buffer manager to add the listener to.
     * @param channel Input channel to read.
     * @param config Tracker settings. Its sample_rate is replaced by Buffers::s_registered_sample_rate.
     */
    static std::shared_ptr<FormantProcessor> listen(BufferManager& manager, uint32_t channel = 0, Kinesis::FormantConfig config = {});

    void processing_function(const std::shared_ptr<Buffer>& buffer) override;

    [[nodiscard]] bool is_compatible_with(const std::shared_ptr<Buffer>& buffer) const override;

    /** @brief The latest reading. Safe from any thread. */
    [[nodiscard]] Kinesis::FormantReading latest() const;

    /** @brief A copy of the frame last analysed, for drawing. Safe from any thread. */
    [[nodiscard]] std::vector<double> frame() const;

    /** @brief A copy of the LPC envelope in dB of the last voiced frame. Empty until one is found. Safe from any thread. */
    [[nodiscard]] std::vector<double> envelope() const;

    /** @brief Frames analysed so far. Stays at zero if no input is arriving. */
    [[nodiscard]] uint64_t frames() const { return m_frames.load(std::memory_order_relaxed); }

    [[nodiscard]] const Kinesis::FormantConfig& config() const { return m_tracker.config(); }

private:
    Kinesis::FormantTracker m_tracker;
    bool m_silence;
    std::vector<double> m_ring;
    uint64_t m_skipped { 0 };

    /// Guards the three published values below. Written only on the processing thread.
    mutable Memory::Seqlock m_published;
    Kinesis::FormantReading m_reading;
    std::vector<double> m_frame;
    std::vector<double> m_envelope;
    bool m_has_envelope { false };

    std::atomic<uint64_t> m_frames { 0 };

    template <typename Copy>
    auto read_published(Copy copy) const;
};

} // namespace MayaFlux::Buffers
