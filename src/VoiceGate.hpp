#pragma once

#include <MayaFlux/Kinesis/MotionChannel.hpp>

namespace MayaFlux::Kinesis {

/**
 * @struct VoiceGateConfig
 * @brief The numbers that tune a VoiceGate.
 */
struct VoiceGateConfig {
    size_t floor_window { 64 }; ///< Frames the noise floor estimate looks back over.
    double margin { 0.08 }; ///< Level above the floor, on the 0..1 level scale, that counts as sound. 0.08 is about 5 dB.
    double min_periodicity { 0.35 }; ///< Autocorrelation peak that counts as pitched.
    size_t vote_window { 6 }; ///< Recent frames that vote on voice.
    size_t warm_frames { 100 }; ///< Frames spent learning the floor before any frame can be voice.
    double onset_rate { 4.0 }; ///< Rise of the level per second that counts as an onset.
};

/**
 * @struct VoiceActivity
 * @brief What a VoiceGate found in one frame.
 */
struct VoiceActivity {
    bool voice { false };
    bool warming { true }; ///< Still learning the noise floor.
    bool onset { false }; ///< The level is rising fast: a sound has just started.
    double level { 0.0 }; ///< 0..1, from -80 dB to -20 dB.
    double floor { 0.0 }; ///< The learned noise floor, same scale.
    double periodicity { 0.0 };
    double speed { 0.0 }; ///< How fast the point is moving on the plane, per second.
};

/**
 * @class VoiceGate
 * @brief Tells a voice from steady noise such as a fan, one frame at a time.
 *
 * A fan is loud but steady and has no pitch. A voice is loud, pitched, and always moving. Each frame is
 * turned into one point on a plane and the point is watched:
 *
 *   x = how far the level is above the noise floor   (Stochastic::Estimate, QUIET_PERIOD_FLOOR)
 *   y = how pitched the frame is                     (peak of the normalised autocorrelation)
 *
 * A MotionChannel cleans the point and differentiates it. Its SymbolicTrajectory observes the point
 * through a 2 by 2 lattice, whose cells are the four quadrants of the plane:
 *
 *           pitched | cell (0,1) quiet voice      cell (1,1) VOICE
 *                   |
 *         unpitched | cell (0,0) room, fan        cell (1,0) loud noise
 *                   +---------------------------------------------------
 *                          below floor              above floor
 *
 * A frame counts as voice when most of the recent frames sit in cell (1,1). That majority vote keeps a
 * single click or the gap between words from flipping the answer.
 *
 * The gate needs about warm_frames frames of quiet at the start to learn the floor. It takes the two
 * numbers it cannot compute itself (RMS and periodicity) as arguments, so it works on any frame source.
 */
class VoiceGate {
public:
    explicit VoiceGate(VoiceGateConfig config = {});

    /**
     * @param rms RMS of the frame.
     * @param periodicity Peak of the normalised autocorrelation in the pitch range, 0..1.
     * @param dt Seconds since the previous frame.
     */
    VoiceActivity update(double rms, double periodicity, double dt);

private:
    VoiceGateConfig m_config;
    Stochastic::Estimate m_floor;
    MotionChannel<glm::vec2> m_motion;
    size_t m_frames { 0 };
};

} // namespace MayaFlux::Kinesis
