#include "VoiceInput.hpp"

#include <MayaFlux/Buffers/BufferProcessingChain.hpp>

namespace MayaFlux::Buffers {

std::shared_ptr<VoiceInput> VoiceInput::listen(BufferManager& manager, VoiceInputConfig config)
{
    auto input = std::make_shared<VoiceInput>();
    config.formants.sample_rate = config.sample_rate;
    input->m_config = config;

    input->m_buffer = std::make_shared<AudioBuffer>(config.channel);
    manager.add_buffer(input->m_buffer, ProcessingToken::AUDIO_BACKEND, config.channel);
    manager.register_input_listener(input->m_buffer, config.channel);

    input->m_recorder = std::make_shared<RingRecorder>(config.ring_seconds, config.sample_rate);
    input->m_recorder->set_skip_silent_blocks(true);

    input->m_envelope = std::make_shared<EnvelopeProcessor>(config.attack, config.release, config.mode);
    input->m_envelope->set_skip_silent_blocks(true);

    // The tracker silences the block, so it goes last.
    input->m_formants = std::make_shared<FormantProcessor>(config.formants, true);

    auto chain = input->m_buffer->get_processing_chain();
    chain->add_processor(input->m_recorder, input->m_buffer);
    chain->add_processor(input->m_envelope, input->m_buffer);
    chain->add_processor(input->m_formants, input->m_buffer);

    return input;
}

} // namespace MayaFlux::Buffers
