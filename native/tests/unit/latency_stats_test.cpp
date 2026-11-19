#include "test_main.h"

#include "core/clock.h"
#include "core/latency_stats.h"

using htmlppt::Duration;
using htmlppt::LatencyStats;

HTMLPPT_TEST_CASE(latency_stats_report_percentiles) {
  LatencyStats stats;
  for (int i = 1; i <= 100; ++i) stats.Add(Duration{i});
  HTMLPPT_CHECK_EQ(stats.count(), std::size_t{100});
  HTMLPPT_CHECK_EQ(stats.P50(), Duration{51});
  // Nearest rank: the 99th sample of 1..100 is 99, not the maximum.
  HTMLPPT_CHECK_EQ(stats.P99(), Duration{99});
  HTMLPPT_CHECK_EQ(stats.Max(), Duration{100});
}

HTMLPPT_TEST_CASE(latency_stats_empty_sample_is_zero) {
  LatencyStats stats;
  HTMLPPT_CHECK_EQ(stats.count(), std::size_t{0});
  HTMLPPT_CHECK_EQ(stats.P50(), Duration{0});
  HTMLPPT_CHECK_EQ(stats.P99(), Duration{0});
  HTMLPPT_CHECK_EQ(stats.Max(), Duration{0});
}

HTMLPPT_TEST_CASE(latency_stats_keep_the_newest_samples_in_a_ring) {
  LatencyStats stats(4);
  for (const int value : {100, 200, 300, 400}) stats.Add(Duration{value});
  for (const int value : {1, 2}) stats.Add(Duration{value});  // evict 100, 200
  HTMLPPT_CHECK_EQ(stats.count(), std::size_t{6});
  // Only the newest four samples are kept: {300, 400, 1, 2}.
  HTMLPPT_CHECK_EQ(stats.Max(), Duration{400});
  HTMLPPT_CHECK_EQ(stats.P50(), Duration{300});
  HTMLPPT_CHECK_EQ(stats.P99(), Duration{400});
}
