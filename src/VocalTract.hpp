#pragma once

#include <MayaFlux/Nodes/Network/NodeNetwork.hpp>

namespace MayaFlux::Kinesis {

/**
 * @struct TractShape
 * @brief A vowel as three articulations, each 0..1.
 *
 * The three dimensions of the vowel trapezoid: how far forward the tongue constriction sits, how
 * high (narrow) it is, and how rounded the lips are.
 */
struct TractShape {
    double front { 0.5 }; ///< 0 back (pharynx) .. 1 front (hard palate).
    double height { 0.5 }; ///< 0 open .. 1 close.
    double rounding { 0.0 }; ///< 0 spread .. 1 rounded.
};

/**
 * @struct VowelTarget
 * @brief A vowel symbol and its shape.
 */
struct VowelTarget {
    const char* symbol;
    TractShape shape;
};

/// The seven oral vowels of Italian: i e ɛ a ɔ o u.
[[nodiscard]] const std::vector<VowelTarget>& vowel_targets();

/// The vowel target for @p symbol ("i", "e", "E", "a", "O", "o", "u"), or nothing.
[[nodiscard]] std::optional<VowelTarget> vowel_target(std::string_view symbol);

/**
 * @brief The vowels of a written sentence, in order: its vocalic skeleton.
 *
 * Reads Latin script. a e i o u count, y counts as i, grave accents mark the open vowels (è ò give
 * ɛ ɔ, as in Italian), acute and other accents keep the plain vowel. Everything else is skipped.
 */
[[nodiscard]] std::vector<VowelTarget> vowels_in(std::string_view sentence);

/**
 * @brief Tract diameters in cm, glottis first, lips last, for @p shape.
 *
 * A short narrow larynx, then a tube of rest width with one smooth tongue constriction and a lip
 * opening. A front, high tongue narrows the palate (as in i); a back, low one narrows the pharynx and
 * opens the mouth (as in a); rounding narrows the last sections (as in u).
 */
[[nodiscard]] std::vector<double> tract_diameters(const TractShape& shape, size_t sections);

/**
 * @brief Magnitude response of a tract in dB, from glottis to lips, at @p bins frequencies up to @p top_hz.
 *
 * The tract is run as the same Kelly-Lochbaum waveguide VocalTract uses, on an impulse.
 * @p waveguide_rate is the rate the waveguide steps at: VocalTract steps twice per sample, so pass
 * twice the sample rate to match what it sounds like.
 */
[[nodiscard]] std::vector<double> tract_response_db(std::span<const double> diameters, double waveguide_rate, size_t bins, double top_hz);

/// The first @p count peaks of a response from tract_response_db, in Hz.
[[nodiscard]] std::vector<double> tract_formants(std::span<const double> response_db, double top_hz, size_t count = 3);

} // namespace MayaFlux::Kinesis

namespace MayaFlux::Nodes::Network {

/**
 * @class VocalTract
 * @brief A vocal tract as a Kelly-Lochbaum waveguide: a tube of many short sections.
 *
 * WaveguideNetwork's TUBE is one uniform bore. A vocal tract changes width along its length, and the
 * width is the vowel, so it is modelled here as a chain of short tubes. Each section keeps the same
 * two rails WaveguideNetwork uses, a wave travelling toward the lips and one travelling back toward
 * the glottis, and at every joint part of the wave is reflected by how much the area changes:
 *
 *   k = (A[i] - A[i+1]) / (A[i] + A[i+1])
 *
 * The glottis end reflects most of the returning wave and adds the excitation; the lip end reflects
 * with inverted sign and radiates the rest, which is the output.
 *
 * Each section is a node of the network: get_node_output(i) is the pressure in section i, so the
 * standing wave inside the tract can be drawn. The shape can be set from any thread and is reached
 * smoothly, as articulators move. The network runs its waveguide at twice the sample rate, which
 * makes the default 44 sections about 16 cm long, the length of an adult tract.
 *
 * Excite it with any node, for example a GlottalSource for voice or noise for a whisper.
 */
class VocalTract : public NodeNetwork {
public:
    explicit VocalTract(size_t sections = 44, double sample_rate = 48000.0);

    void process_batch(unsigned int num_samples) override;
    [[nodiscard]] size_t get_node_count() const override { return m_sections; }
    [[nodiscard]] std::optional<std::vector<double>> get_audio_buffer() const override;
    [[nodiscard]] std::optional<double> get_node_output(size_t index) const override;

    void set_exciter(std::shared_ptr<Node> exciter) { m_exciter = std::move(exciter); }

    /// Aim for @p diameters (cm, glottis first). Any thread.
    void set_target(std::span<const double> diameters);

    /// Aim for @p shape. Any thread.
    void set_shape(const Kinesis::TractShape& shape) { set_target(Kinesis::tract_diameters(shape, m_sections)); }

    /// Seconds for the tract to cover 1 - 1/e of a change of shape.
    void set_movement(double seconds) { m_movement.store(std::max(seconds, 0.0), std::memory_order_relaxed); }

    /// The diameters the tract has now, glottis first. Any thread.
    [[nodiscard]] std::vector<double> diameters() const;

    /// The pressure in every section now, glottis first. Any thread.
    [[nodiscard]] std::vector<double> pressures() const;

private:
    size_t m_sections;
    double m_rate;
    std::shared_ptr<Node> m_exciter;

    std::vector<double> m_right; ///< Toward the lips.
    std::vector<double> m_left; ///< Toward the glottis.
    std::vector<double> m_next_right;
    std::vector<double> m_next_left;
    std::vector<double> m_reflection;
    std::vector<double> m_current;

    std::vector<std::atomic<double>> m_target;
    std::vector<std::atomic<double>> m_shown_diameter;
    std::vector<std::atomic<double>> m_shown_pressure;
    std::atomic<double> m_movement { 0.04 };

    static constexpr double k_glottal_reflection = 0.75;
    static constexpr double k_lip_reflection = -0.85;
    static constexpr double k_loss = 0.999;

    void update_reflections();
    double step(double excitation);
};

} // namespace MayaFlux::Nodes::Network
