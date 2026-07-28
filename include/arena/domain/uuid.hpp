#pragma once

#include <string>

namespace arena::domain {

[[nodiscard]] std::string uuid_v7();
[[nodiscard]] std::string utc_now();
[[nodiscard]] bool is_uuid(const std::string& value);

}  // namespace arena::domain
