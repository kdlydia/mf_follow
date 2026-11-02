#include "Lpc.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace MayaFlux::Kinesis::Discrete {

std::vector<double> levinson_durbin(std::span<const double> autocorrelation, size_t order)
{
    std::vector<double> r(autocorrelation.begin(), autocorrelation.end());
    r.resize(std::max(r.size(), order + 1), 0.0);

    std::vector<double> a(order + 1, 0.0);
    a[0] = 1.0;
    if (r.empty() || r[0] <= 0.0) {
        return a;
    }

    std::vector<double> previous(order + 1, 0.0);
    double error = r[0];
    for (size_t i = 1; i <= order && error > 0.0; ++i) {
        double sum = r[i];
        for (size_t j = 1; j < i; ++j) {
            sum += a[j] * r[i - j];
        }
        const double reflection = -sum / error;

        previous = a;
        for (size_t j = 1; j < i; ++j) {
            a[j] = previous[j] + reflection * previous[i - j];
        }
        a[i] = reflection;
        error *= 1.0 - reflection * reflection;
    }
    return a;
}

std::vector<double> lpc_envelope_db(std::span<const double> a, size_t points)
{
    points = std::max<size_t>(points, 2);
    std::vector<double> envelope(points);
    for (size_t k = 0; k < points; ++k) {
        const double w = std::numbers::pi * static_cast<double>(k) / static_cast<double>(points - 1);
        double re = 1.0;
        double im = 0.0;
        for (size_t j = 1; j < a.size(); ++j) {
            re += a[j] * std::cos(w * static_cast<double>(j));
            im -= a[j] * std::sin(w * static_cast<double>(j));
        }
        envelope[k] = -10.0 * std::log10(std::max(re * re + im * im, 1e-12));
    }
    return envelope;
}

std::vector<double> spectral_peaks(std::span<const double> curve, double span_hz)
{
    std::vector<double> found;
    if (curve.size() < 3) {
        return found;
    }

    const double step_hz = span_hz / static_cast<double>(curve.size() - 1);
    for (size_t k = 1; k + 1 < curve.size(); ++k) {
        if (curve[k] > curve[k - 1] && curve[k] >= curve[k + 1]) {
            const double bend = curve[k - 1] - 2.0 * curve[k] + curve[k + 1];
            const double offset = bend != 0.0 ? 0.5 * (curve[k - 1] - curve[k + 1]) / bend : 0.0;
            found.push_back((static_cast<double>(k) + offset) * step_hz);
        }
    }
    return found;
}

} // namespace MayaFlux::Kinesis::Discrete
