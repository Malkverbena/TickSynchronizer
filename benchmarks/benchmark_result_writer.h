// Declares benchmark report serialization functions.
// Allows the runner and scripts to persist machine-readable results.

#pragma once

#include "benchmark_result.h"

#include <iosfwd>
#include <string>

namespace tick_synchronizer::benchmarks {

// Writes the complete provenance-rich report as stable JSON.
void write_json_report(const ProtocolBenchmarkReport &report, std::ostream &output);

// Writes flattened dataset operation metrics for tabular analysis.
void write_csv_report(const ProtocolBenchmarkReport &report, std::ostream &output);

// Creates or replaces a JSON report file and reports I/O success.
bool write_json_report_file(const ProtocolBenchmarkReport &report, const std::string &path);

// Creates or replaces a CSV report file and reports I/O success.
bool write_csv_report_file(const ProtocolBenchmarkReport &report, const std::string &path);

} // namespace tick_synchronizer::benchmarks
