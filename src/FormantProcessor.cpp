#include "FormantProcessor.hpp"

#include <MayaFlux/Buffers/BufferProcessingChain.hpp>
#include <MayaFlux/Buffers/BufferSpec.hpp>

namespace MayaFlux::Buffers {

FormantProcessor::FormantProcessor(Kinesis::FormantConfig config, bool silence_buffer)
    : m_tracker(config)
    , m_silence(silence_buffer)
    , m_ring(config.frame_size, 0.0)
    , m_frame(config.frame_size, 0.0)
    , m_envelope(config.grid_points, 0.0)
{
}

std::shared_ptr<FormantProcessor> FormantProcessor::listen(BufferManager& manager, uint32_t channel, Kinesis::FormantConfig config)
{
    config.sample_rate = static_cast<double>(s_registered_sample_rate);

    auto input = std::make_shared<AudioBuffer>(channel);
    manager.add_buffer(input, ProcessingToken::AUDIO_BACKEND, channel);
    manager.register_input_listener(input, channel);

    auto processor = std::make_shared<FormantProcessor>(config);
    input->get_processing_chain()->add_processor(processor, input);
    return processor;
}

bool FormantProcessor::is_compatible_with(const std::shared_ptr<Buffer>& buffer) const
{
    return std::dynamic_pointer_cast<AudioBuffer>(buffer) != nullptr;
}

void FormantProcessor::processing_function(const std::shared_ptr<Buffer>& buffer)
{
    auto audio = std::dynamic_pointer_cast<AudioBuffer>(buffer);
    if (!audio) {
        return;
    }

    auto& data = audio->get_data();
    if (std::ranges::all_of(data, [](double v) { return v == 0.0; })) {
        ++m_skipped;
        return;
    }

    const size_t size = m_ring.size();
    if (data.size() >= size) {
        std::ranges::copy(std::span<const double>(data).last(size), m_ring.begin());
    } else {
        std::ranges::rotate(m_ring, m_ring.begin() + static_cast<std::ptrdiff_t>(data.size()));
        std::ranges::copy(data, m_ring.end() - static_cast<std::ptrdiff_t>(data.size()));
    }

    // The hop is the time since the last block that carried sound, empty cycles included.
    const double cycles = static_cast<double>(m_skipped + 1);
    m_skipped = 0;
    const double hop = cycles * static_cast<double>(data.size()) / m_tracker.config().sample_rate;

    const Kinesis::FormantReading reading = m_tracker.analyse(m_ring, hop);
    const auto& envelope = m_tracker.envelope();

    m_published.begin_write();
    m_reading = reading;
    std::ranges::copy(m_ring, m_frame.begin());
    if (reading.voiced && envelope.size() == m_envelope.size()) {
        std::ranges::copy(envelope, m_envelope.begin());
        m_has_envelope = true;
    }
    m_published.end_write();

    m_frames.fetch_add(1, std::memory_order_relaxed);

    if (m_silence) {
        std::ranges::fill(data, 0.0);
    }
}

template <typename Copy>
auto FormantProcessor::read_published(Copy copy) const
{
    decltype(copy()) result;
    uint32_t seq {};
    do {
        seq = m_published.read_begin();
        result = copy();
    } while (m_published.read_retry(seq));
    return result;
}

Kinesis::FormantReading FormantProcessor::latest() const
{
    return read_published([this] { return m_reading; });
}

std::vector<double> FormantProcessor::frame() const
{
    return read_published([this] { return m_frame; });
}

std::vector<double> FormantProcessor::envelope() const
{
    return read_published([this] { return m_has_envelope ? m_envelope : std::vector<double> {}; });
}

} // namespace MayaFlux::Buffers
