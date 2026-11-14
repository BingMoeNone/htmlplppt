#include "test_main.h"

#include "core/clock.h"
#include "core/retry_policy.h"

using htmlppt::Duration;
using htmlppt::FakeClock;
using htmlppt::RestartBudget;
using htmlppt::RetryConfig;
using htmlppt::RetryPolicy;

HTMLPPT_TEST_CASE(retry_backoff_is_capped) {
  RetryConfig config;
  config.max_attempts = 3;
  config.base_delay = Duration{100};
  config.max_delay = Duration{300};
  config.jitter_ratio = 0.0;

  RetryPolicy policy(config);
  const auto first = policy.next_delay(0);
  const auto second = policy.next_delay(1);
  const auto third = policy.next_delay(2);

  HTMLPPT_CHECK(first.has_value());
  HTMLPPT_CHECK(second.has_value());
  HTMLPPT_CHECK(third.has_value());
  HTMLPPT_CHECK_EQ(first->count(), 100);
  HTMLPPT_CHECK_EQ(second->count(), 200);
  HTMLPPT_CHECK_EQ(third->count(), 300);
  HTMLPPT_CHECK(!policy.next_delay(3).has_value());
}

HTMLPPT_TEST_CASE(retry_jitter_is_bounded_and_deterministic) {
  RetryConfig config;
  config.max_attempts = 3;
  config.base_delay = Duration{100};
  config.max_delay = Duration{1000};
  config.jitter_ratio = 0.5;
  config.seed = 42;

  RetryPolicy first_policy(config);
  RetryPolicy second_policy(config);

  const auto first = first_policy.next_delay(0);
  const auto second = second_policy.next_delay(0);
  HTMLPPT_CHECK(first.has_value());
  HTMLPPT_CHECK(second.has_value());
  HTMLPPT_CHECK(first->count() >= 50);
  HTMLPPT_CHECK(first->count() <= 150);
  HTMLPPT_CHECK_EQ(first->count(), second->count());

  first_policy.reset();
  const auto after_reset = first_policy.next_delay(0);
  HTMLPPT_CHECK(after_reset.has_value());
  HTMLPPT_CHECK_EQ(after_reset->count(), first->count());
}

HTMLPPT_TEST_CASE(restart_budget_expires_old_entries) {
  FakeClock clock;
  RestartBudget budget(2, Duration{10000});

  HTMLPPT_CHECK(budget.allow(clock.now()));
  budget.record(clock.now());
  HTMLPPT_CHECK(budget.allow(clock.now()));
  budget.record(clock.now());
  HTMLPPT_CHECK(!budget.allow(clock.now()));

  clock.advance(Duration{10001});
  HTMLPPT_CHECK(budget.allow(clock.now()));
}
