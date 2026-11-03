#include "VocalTract.hpp"

namespace MayaFlux::Kinesis {

namespace {

    constexpr double k_rest_diameter = 2.4; ///< cm, an open tube.
    constexpr double k_larynx_diameter = 0.8;
    constexpr double k_larynx_end = 0.08; ///< Fraction of the length that is larynx.
    constexpr double k_lip_start = 0.86; ///< Fraction where the lips begin.
    constexpr double k_glottal = 0.75; ///< Same ends and loss as VocalTract.
    constexpr double k_lip = -0.85;
    constexpr double k_tube_loss = 0.999;

    /// The tract as the network runs it, with fixed areas, for measuring its response.
    struct StaticTract {
        std::vector<double> right, left, next_right, next_left, reflection;

        explicit StaticTract(std::span<const double> diameters)
            : right(diameters.size(), 0.0)
            , left(diameters.size(), 0.0)
            , next_right(diameters.size(), 0.0)
            , next_left(diameters.size(), 0.0)
            , reflection(diameters.size() > 1 ? diameters.size() - 1 : 0, 0.0)
        {
            for (size_t i = 0; i + 1 < diameters.size(); ++i) {
                const double a = diameters[i] * diameters[i];
                const double b = diameters[i + 1] * diameters[i + 1];
                reflection[i] = a + b > 0.0 ? (a - b) / (a + b) : 0.0;
            }
        }
    };

    /// One step of the Kelly-Lochbaum chain. Returns what leaves the lips.
    double kelly_lochbaum(std::vector<double>& right, std::vector<double>& left,
        std::vector<double>& next_right, std::vector<double>& next_left,
        std::span<const double> reflection, double excitation, double glottal, double lip, double loss)
    {
        const size_t n = right.size();
        next_right[0] = left[0] * glottal + excitation;
        next_left[n - 1] = right[n - 1] * lip;

        for (size_t i = 0; i + 1 < n; ++i) {
            const double w = reflection[i] * (right[i] + left[i + 1]);
            next_right[i + 1] = right[i] - w;
            next_left[i] = left[i + 1] + w;
        }

        for (size_t i = 0; i < n; ++i) {
            right[i] = next_right[i] * loss;
            left[i] = next_left[i] * loss;
        }
        return right[n - 1] * (1.0 + lip);
    }

} // namespace

const std::vector<VowelTarget>& vowel_targets()
{
    static const std::vector<VowelTarget> targets {
        { "i", { .front = 0.95, .height = 0.95, .rounding = 0.0 } },
        { "e", { .front = 0.85, .height = 0.65, .rounding = 0.0 } },
        { "E", { .front = 0.75, .height = 0.4, .rounding = 0.0 } },
        { "a", { .front = 0.1, .height = 0.05, .rounding = 0.0 } },
        { "O", { .front = 0.1, .height = 0.35, .rounding = 0.5 } },
        { "o", { .front = 0.1, .height = 0.6, .rounding = 0.75 } },
        { "u", { .front = 0.15, .height = 0.9, .rounding = 0.95 } },
    };
    return targets;
}

std::optional<VowelTarget> vowel_target(std::string_view symbol)
{
    for (const auto& target : vowel_targets())
        if (symbol == target.symbol)
            return target;
    return std::nullopt;
}

std::vector<VowelTarget> vowels_in(std::string_view sentence)
{
    std::vector<VowelTarget> out;
    const auto push = [&out](const char* symbol) {
        if (auto t = vowel_target(symbol))
            out.push_back(*t);
    };

    for (size_t i = 0; i < sentence.size(); ++i) {
        const auto c = static_cast<unsigned char>(sentence[i]);
        if (c < 0x80) {
            switch (std::tolower(c)) {
            case 'a': push("a"); break;
            case 'e': push("e"); break;
            case 'i':
            case 'y': push("i"); break;
            case 'o': push("o"); break;
            case 'u': push("u"); break;
            default: break;
            }
            continue;
        }
        // Two-byte UTF-8 Latin-1 letters: 0xC3 followed by the letter.
        if (c == 0xC3 && i + 1 < sentence.size()) {
            // C3 xx encodes U+00C0..U+00FF as xx + 0x40; | 0x20 folds upper case into lower.
            const auto d = static_cast<unsigned>((static_cast<unsigned char>(sentence[++i]) + 0x40) | 0x20);
            switch (d) {
            case 0xE0: case 0xE1: case 0xE2: case 0xE4: push("a"); break;
            case 0xE8: push("E"); break;
            case 0xE9: case 0xEA: case 0xEB: push("e"); break;
            case 0xEC: case 0xED: case 0xEE: case 0xEF: push("i"); break;
            case 0xF2: push("O"); break;
            case 0xF3: case 0xF4: case 0xF6: push("o"); break;
            case 0xF9: case 0xFA: case 0xFB: case 0xFC: push("u"); break;
            default: break;
            }
        }
    }
    return out;
}

std::vector<double> tract_diameters(const TractShape& shape, size_t sections)
{
    const double front = std::clamp(shape.front, 0.0, 1.0);
    const double height = std::clamp(shape.height, 0.0, 1.0);
    const double rounding = std::clamp(shape.rounding, 0.0, 1.0);

    // Front vowels constrict the hard palate. Back vowels constrict further back the more open they
    // are: the velum for u, the uvula for o, the pharynx for a.
    const double back_centre = 0.30 + 0.34 * height;
    const double centre = front * 0.80 + (1.0 - front) * back_centre;
    const double narrowest = 0.3 + 1.6 * (1.0 - height) * (0.4 + 0.6 * front);
    constexpr double width = 0.34;
    // Open vowels drop the jaw and widen the mouth; rounding closes the lips.
    const double mouth = k_rest_diameter * (1.0 + 0.35 * (1.0 - height));
    const double lips = std::max(0.25, mouth * (1.0 - 0.85 * rounding));

    std::vector<double> d(sections);
    for (size_t s = 0; s < sections; ++s) {
        const double x = sections > 1 ? static_cast<double>(s) / static_cast<double>(sections - 1) : 0.0;
        double value = k_rest_diameter;
        if (x < k_larynx_end)
            value = k_larynx_diameter + (k_rest_diameter - k_larynx_diameter) * (x / k_larynx_end);
        if (x > 0.55)
            value += (mouth - k_rest_diameter) * std::min(1.0, (x - 0.55) / 0.3);

        const double distance = std::abs(x - centre);
        if (distance < width * 0.5) {
            const double bump = std::cos(std::numbers::pi * distance / width);
            value = value - (value - narrowest) * bump * bump;
        }
        if (x >= k_lip_start)
            value += (lips - value) * std::min(1.0, 2.0 * (x - k_lip_start) / (1.0 - k_lip_start + 1e-9));
        d[s] = std::max(0.05, value);
    }
    return d;
}

std::vector<double> tract_response_db(std::span<const double> diameters, double rate, size_t bins, double top_hz)
{
    if (diameters.size() < 2 || bins == 0)
        return std::vector<double>(bins, -120.0);

    StaticTract tract(diameters);
    constexpr size_t length = 4096;
    std::vector<double> impulse(length);
    for (size_t t = 0; t < length; ++t)
        impulse[t] = kelly_lochbaum(tract.right, tract.left, tract.next_right, tract.next_left,
            tract.reflection, t == 0 ? 1.0 : 0.0, k_glottal, k_lip, k_tube_loss);

    std::vector<double> db(bins);
    for (size_t b = 0; b < bins; ++b) {
        const double w = 2.0 * std::numbers::pi * (top_hz * static_cast<double>(b) / static_cast<double>(bins - 1)) / rate;
        std::complex<double> sum {};
        for (size_t t = 0; t < length; ++t)
            sum += impulse[t] * std::polar(1.0, -w * static_cast<double>(t));
        db[b] = 20.0 * std::log10(std::max(std::abs(sum), 1e-9));
    }
    return db;
}

std::vector<double> tract_formants(std::span<const double> response_db, double top_hz, size_t count)
{
    std::vector<double> hz;
    if (response_db.size() < 3)
        return hz;
    const double step = top_hz / static_cast<double>(response_db.size() - 1);
    // A formant stands out: at least 2 dB above the lowest point within about 150 Hz either side.
    const auto reach = static_cast<size_t>(std::max(2.0, 150.0 / step));
    const auto prominent = [&](size_t i) {
        const size_t lo = i > reach ? i - reach : 0;
        const size_t hi = std::min(response_db.size() - 1, i + reach);
        const double floor_left = *std::min_element(response_db.begin() + static_cast<std::ptrdiff_t>(lo), response_db.begin() + static_cast<std::ptrdiff_t>(i));
        const double floor_right = *std::min_element(response_db.begin() + static_cast<std::ptrdiff_t>(i), response_db.begin() + static_cast<std::ptrdiff_t>(hi) + 1);
        return response_db[i] - std::max(floor_left, floor_right) >= 2.0;
    };
    for (size_t i = 1; i + 1 < response_db.size() && hz.size() < count; ++i) {
        if (response_db[i] > response_db[i - 1] && response_db[i] >= response_db[i + 1] && prominent(i)) {
            // Parabolic refinement of the peak position.
            const double a = response_db[i - 1], b = response_db[i], c = response_db[i + 1];
            const double denom = a - 2.0 * b + c;
            const double offset = std::abs(denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0;
            const double f = (static_cast<double>(i) + offset) * step;
            if (f > 150.0)
                hz.push_back(f);
        }
    }
    return hz;
}

} // namespace MayaFlux::Kinesis

namespace MayaFlux::Nodes::Network {

VocalTract::VocalTract(size_t sections, double sample_rate)
    : m_sections(std::max<size_t>(sections, 2))
    , m_rate(sample_rate)
    , m_right(m_sections, 0.0)
    , m_left(m_sections, 0.0)
    , m_next_right(m_sections, 0.0)
    , m_next_left(m_sections, 0.0)
    , m_reflection(m_sections - 1, 0.0)
    , m_current(Kinesis::tract_diameters({}, m_sections))
    , m_target(m_sections)
    , m_shown_diameter(m_sections)
    , m_shown_pressure(m_sections)
{
    for (size_t i = 0; i < m_sections; ++i) {
        m_target[i].store(m_current[i]);
        m_shown_diameter[i].store(m_current[i]);
    }
    update_reflections();
}

void VocalTract::set_target(std::span<const double> diameters)
{
    for (size_t i = 0; i < m_sections && i < diameters.size(); ++i)
        m_target[i].store(std::max(0.05, diameters[i]), std::memory_order_relaxed);
}

std::vector<double> VocalTract::diameters() const
{
    std::vector<double> out(m_sections);
    for (size_t i = 0; i < m_sections; ++i)
        out[i] = m_shown_diameter[i].load(std::memory_order_relaxed);
    return out;
}

std::vector<double> VocalTract::pressures() const
{
    std::vector<double> out(m_sections);
    for (size_t i = 0; i < m_sections; ++i)
        out[i] = m_shown_pressure[i].load(std::memory_order_relaxed);
    return out;
}

std::optional<double> VocalTract::get_node_output(size_t index) const
{
    if (index >= m_sections)
        return std::nullopt;
    return m_shown_pressure[index].load(std::memory_order_relaxed);
}

std::optional<std::vector<double>> VocalTract::get_audio_buffer() const
{
    if (m_last_audio_buffer.empty())
        return std::nullopt;
    return m_last_audio_buffer;
}

void VocalTract::update_reflections()
{
    for (size_t i = 0; i + 1 < m_sections; ++i) {
        const double a = m_current[i] * m_current[i];
        const double b = m_current[i + 1] * m_current[i + 1];
        m_reflection[i] = a + b > 0.0 ? (a - b) / (a + b) : 0.0;
    }
}

double VocalTract::step(double excitation)
{
    // Two waveguide steps per sample: the tube runs at twice the sample rate.
    double out = 0.0;
    for (int half = 0; half < 2; ++half)
        out += Kinesis::kelly_lochbaum(m_right, m_left, m_next_right, m_next_left, m_reflection,
            excitation * 0.5, k_glottal_reflection, k_lip_reflection, k_loss);
    return out * 0.5;
}

void VocalTract::process_batch(unsigned int num_samples)
{
    // The articulators move once per block, toward the target.
    const double movement = m_movement.load(std::memory_order_relaxed);
    const double block_seconds = static_cast<double>(num_samples) / m_rate;
    const double approach = movement > 0.0 ? 1.0 - std::exp(-block_seconds / movement) : 1.0;
    for (size_t i = 0; i < m_sections; ++i)
        m_current[i] += (m_target[i].load(std::memory_order_relaxed) - m_current[i]) * approach;
    update_reflections();

    thread_local std::vector<double> scratch;
    scratch.assign(num_samples, 0.0);
    for (unsigned int s = 0; s < num_samples; ++s) {
        const double excitation = m_exciter ? m_exciter->process_sample(0.0) : 0.0;
        scratch[s] = step(excitation);
    }

    for (size_t i = 0; i < m_sections; ++i) {
        m_shown_diameter[i].store(m_current[i], std::memory_order_relaxed);
        m_shown_pressure[i].store(m_right[i] + m_left[i], std::memory_order_relaxed);
    }

    while (m_audio_buffer_lock.test_and_set(std::memory_order_acquire))
        std::this_thread::yield();
    m_last_audio_buffer.assign(scratch.begin(), scratch.end());
    apply_output_scale();
    m_audio_buffer_lock.clear(std::memory_order_release);
}

} // namespace MayaFlux::Nodes::Network
