#pragma once

#include "EnvelopeProcessor.hpp"
#include "FormantProcessor.hpp"
#include "RingRecorder.hpp"

#include <MayaFlux/Buffers/BufferManager.hpp>

namespace MayaFlux::Buffers {

/**
 * @struct VoiceInputConfig
 * @brief How a VoiceInput listens.
 */
struct VoiceInputConfig {
    uint32_t channel { 0 }; ///< Input channel to listen to. The listener sits on the output channel of the same number.
    double sample_rate { 48000.0 }; ///< Rate of the input.
    double ring_seconds { 30.0 }; ///< How much of the past the recorder keeps.
    double attack { 0.005 }; ///< Envelope attack, seconds.
    double release { 0.15 }; ///< Envelope release, seconds.
    Kinesis::Discrete::EnvelopeMode mode { Kinesis::Discrete::EnvelopeMode::RMS };
    Kinesis::FormantConfig formants {}; ///< Its sample_rate is replaced by sample_rate above.
};

/**
 * @class VoiceInput
 * @brief One microphone, recorded, followed and tracked, through one buffer.
 *
 * Makes an input listener on @p manager and gives its chain a fixed order: a RingRecorder first,
 * so it records the block as it arrived, then an EnvelopeProcessor, then a FormantProcessor, which
 * silences the block last so the microphone never reaches the speakers. Each piece is exposed for
 * use on its own: takes and lag taps from recorder(), loudness from envelope(), formants from
 * formants().
 *
 * The manager is the caller's, so this works with however the application initialised MayaFlux.
 */
class VoiceInput {
public:
    /**
     * @brief Listen to an input channel of @p manager.
     *
     * Enable the input in the stream settings first.
     */
    static std::shared_ptr<VoiceInput> listen(BufferManager& manager, VoiceInputConfig config = {});

    [[nodiscard]] const std::shared_ptr<AudioBuffer>& buffer() const { return m_buffer; }
    [[nodiscard]] const std::shared_ptr<RingRecorder>& recorder() const { return m_recorder; }
    [[nodiscard]] const std::shared_ptr<EnvelopeProcessor>& envelope() const { return m_envelope; }
    [[nodiscard]] const std::shared_ptr<FormantProcessor>& formants() const { return m_formants; }

    [[nodiscard]] const VoiceInputConfig& config() const { return m_config; }

private:
    VoiceInputConfig m_config;
    std::shared_ptr<AudioBuffer> m_buffer;
    std::shared_ptr<RingRecorder> m_recorder;
    std::shared_ptr<EnvelopeProcessor> m_envelope;
    std::shared_ptr<FormantProcessor> m_formants;
};

} // namespace MayaFlux::Buffers
