// Calculates robust distribution statistics for measured samples.
// Provides median, p95, mean, extrema, and median absolute deviation.

#include "benchmark_statistics.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace tick_synchronizer::benchmarks {
namespace {

// Interpolates one percentile from an already sorted sample vector.
double percentile_sorted(const std::vector<double> &values, double fraction) {
	if (values.empty()) {
		return 0.0;
	}
	if (values.size() == 1) {
		return values.front();
	}
	const double position = fraction * static_cast<double>(values.size() - 1);
	const std::size_t lower = static_cast<std::size_t>(std::floor(position));
	const std::size_t upper = static_cast<std::size_t>(std::ceil(position));
	const double weight = position - static_cast<double>(lower);
	return values[lower] * (1.0 - weight) + values[upper] * weight;
}

} // namespace

DistributionStatistics calculate_distribution_statistics(std::vector<double> values) {
	DistributionStatistics statistics;
	statistics.sample_count = values.size();
	if (values.empty()) {
		return statistics;
	}
	std::sort(values.begin(), values.end());
	statistics.minimum = values.front();
	// Interpolates one percentile from an already sorted sample vector.
	statistics.median = percentile_sorted(values, 0.5);
	// Interpolates one percentile from an already sorted sample vector.
	statistics.p95 = percentile_sorted(values, 0.95);
	statistics.maximum = values.back();
	statistics.mean = std::accumulate(values.begin(), values.end(), 0.0) /
			static_cast<double>(values.size());
	std::vector<double> deviations;
	deviations.reserve(values.size());
	for (const double value : values) {
		deviations.push_back(std::fabs(value - statistics.median));
	}
	std::sort(deviations.begin(), deviations.end());
	// Interpolates one percentile from an already sorted sample vector.
	statistics.median_absolute_deviation = percentile_sorted(deviations, 0.5);
	return statistics;
}

} // namespace tick_synchronizer::benchmarks
