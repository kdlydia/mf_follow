#pragma once

#include <MayaFlux/Buffers/AudioBuffer.hpp>
#include <MayaFlux/Buffers/BufferProcessor.hpp>
#include <MayaFlux/Kakshya/Source/DynamicSoundStream.hpp>
#include <MayaFlux/Transitive/Memory/SweepList.hpp>

namespace MayaFlux::Buffers {

/**
 * @class RingRecorder
 * @brief Keeps the most recent seconds of a buffer in a ring, and cuts takes from it.
 *
 * Each block is appended to a circular Kakshya::DynamicSoundStream with append_frames, so the ring's
 * write head moves the way TapSet lag taps and DynamicSoundStream::snapshot expect. (SoundStreamWriter
 * writes at its own position instead and leaves the head alone.)
 *
 * A take is a copy of the last frames, made with snapshot(). Ask for one from any thread with
 * request_take(); the recorder cuts it on the processing thread right after appending, so the copy
 * never races the writer, and hands it back through collect(). Requests and results travel on
 * Memory::SweepList, so no thread ever waits on another.
 *
 * Place it before any processor that changes or silences the block.
 */
class RingRecorder : public BufferProcessor {
public:
    /**
     * @brief Skip blocks that are entirely zero instead of recording them.
     *
     * An input listener is run about twice per microphone delivery, so every other block is empty;
     * recording those would put a gap after every block. Leave off for buffers where silence is real.
     */
    void set_skip_silent_blocks(bool skip) { m_skip_silent.store(skip, std::memory_order_relaxed); }

    /// A finished take: the id request_take() returned, and the copy.
    struct Take {
        uint64_t id {};
        std::shared_ptr<Kakshya::DynamicSoundStream> stream;
    };

    /**
     * @param seconds How much of the past the ring keeps.
     * @param sample_rate Rate of the audio recorded, and of the ring.
     */
    RingRecorder(double seconds, double sample_rate);

    void processing_function(const std::shared_ptr<Buffer>& buffer) override;
    [[nodiscard]] bool is_compatible_with(const std::shared_ptr<Buffer>& buffer) const override;

    /// The ring itself, for TapSet lag taps. Mono.
    [[nodiscard]] const std::shared_ptr<Kakshya::DynamicSoundStream>& ring() const { return m_ring; }

    /// Frames recorded since the start, never wrapping. Mark a moment with it, any thread.
    [[nodiscard]] uint64_t frames() const { return m_frames.load(std::memory_order_acquire); }

    [[nodiscard]] double sample_rate() const { return m_rate; }

    /// Frames the ring holds.
    [[nodiscard]] uint64_t capacity() const { return m_capacity; }

    /**
     * @brief Ask for a take of the last @p frames, capped at the ring's capacity.
     * @return An id to match against collect(). Any thread.
     */
    uint64_t request_take(uint64_t frames);

    /// Ask for a take of everything recorded since @p since_frame (a value of frames()), plus @p preroll.
    uint64_t request_take_since(uint64_t since_frame, uint64_t preroll = 0);

    /// A take that is ready, if any. Any thread.
    [[nodiscard]] std::optional<Take> collect() { return m_results.try_take(); }

private:
    struct Request {
        uint64_t id {};
        uint64_t frames {};
    };

    double m_rate;
    uint64_t m_capacity;
    std::shared_ptr<Kakshya::DynamicSoundStream> m_ring;
    std::atomic<uint64_t> m_frames { 0 };
    std::atomic<uint64_t> m_next_id { 1 };
    std::atomic<bool> m_skip_silent { false };

    Memory::SweepList<Request> m_requests;
    Memory::SweepList<Take> m_results;
};

} // namespace MayaFlux::Buffers
