#include "arena/domain/uuid.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <random>
#include <regex>
#include <sstream>

namespace arena::domain {

std::string uuid_v7() {
    using namespace std::chrono;
    const auto milliseconds_since_epoch =
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

    std::array<std::uint8_t, 16> bytes{};
    std::random_device random;
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(random());
    }

    for (int index = 0; index < 6; ++index) {
        const auto shift = 40 - (index * 8);
        bytes[static_cast<std::size_t>(index)] =
            static_cast<std::uint8_t>((milliseconds_since_epoch >> shift) & 0xff);
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x70U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        output << std::setw(2) << static_cast<unsigned>(bytes[index]);
        if (index == 3 || index == 5 || index == 7 || index == 9) {
            output << '-';
        }
    }
    return output.str();
}

std::string utc_now() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto milliseconds_part =
        duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t time = system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time);
#else
    gmtime_r(&time, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3)
           << std::setfill('0') << milliseconds_part << 'Z';
    return output.str();
}

bool is_uuid(const std::string& value) {
    static const std::regex pattern(
        "^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
        "[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    return std::regex_match(value, pattern);
}

}  // namespace arena::domain
