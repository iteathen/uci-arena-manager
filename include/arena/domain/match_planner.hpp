#pragma once

#include "arena/domain/types.hpp"

#include <stdexcept>

namespace arena::domain {

class PlanError final : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class MatchPlanner final {
  public:
    [[nodiscard]] static MatchPlan normalize_and_schedule(MatchPlan plan);

  private:
    static void validate(const MatchPlan& plan);
    static void schedule_head_to_head(MatchPlan& plan);
    static void schedule_round_robin(MatchPlan& plan);
    static void schedule_gauntlet(MatchPlan& plan);
    static void schedule_swiss_first_round(MatchPlan& plan);
};

}  // namespace arena::domain
