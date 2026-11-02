#include "RingRecorder.hpp"

namespace MayaFlux::Buffers {

RingRecorder::RingRecorder(double seconds, double sample_rate)
    : m_rate(sample_rate)
    , m_capacity(std::max<uint64_t>(1, static_cast<uint64_t>(std::max(seconds, 0.0) * sample_rate)))
    , m_ring(std::make_shared<Kakshya::DynamicSoundStream>(static_cast<uint32_t>(sample_rate), 1))
{
    m_ring->enable_circular_buffer(m_capacity);
}

bool RingRecorder::is_compatible_with(const std::shared_ptr<Buffer>& buffer) const
{
    return std::dynamic_pointer_cast<AudioBuffer>(buffer) != nullptr;
}

uint64_t RingRecorder::request_take(uint64_t frames)
{
    const uint64_t id = m_next_id.fetch_add(1, std::memory_order_relaxed);
    m_requests.push({ .id = id, .frames = std::min(frames, m_capacity) });
    return id;
}

uint64_t RingRecorder::request_take_since(uint64_t since_frame, uint64_t preroll)
{
    const uint64_t now = frames();
    const uint64_t start = since_frame > preroll ? since_frame - preroll : 0;
    return request_take(now > start ? now - start : 0);
}

void RingRecorder::processing_function(const std::shared_ptr<Buffer>& buffer)
{
    auto audio = std::dynamic_pointer_cast<AudioBuffer>(buffer);
    if (!audio)
        return;

    const auto& data = audio->get_data();
    const bool skip = m_skip_silent.load(std::memory_order_relaxed)
        && std::ranges::all_of(data, [](double v) { return v == 0.0; });
    if (!data.empty() && !skip) {
        m_ring->append_frames(data, 0);
        m_frames.fetch_add(data.size(), std::memory_order_release);
    }

    if (m_requests.empty())
        return;

    m_requests.sweep(
        [](Request&) { return true; },
        [this](Request& request) {
            auto copy = request.frames > 0 ? m_ring->snapshot(request.frames) : nullptr;
            m_results.push({ .id = request.id, .stream = std::move(copy) });
        });
}

} // namespace MayaFlux::Buffers
