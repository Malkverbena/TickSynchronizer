// Declares distribution statistics used by benchmark reports.
// Defines the stable statistical output contract for suite version 1.

#pragma once

#include <cstddef>
#include <vector>

namespace tick_synchronizer::benchmarks {

struct DistributionStatistics {
	double minimum = 0.0;
	double median = 0.0;
	double p95 = 0.0;
	double maximum = 0.0;
	double mean = 0.0;
	double median_absolute_deviation = 0.0;
	std::size_t sample_count = 0;
};

// Calculates robust summary statistics from one measured sample set.
DistributionStatistics calculate_distribution_statistics(std::vector<double> values);

} // namespace tick_synchronizer::benchmarks
