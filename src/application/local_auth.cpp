#include "arena/application/local_auth.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <ranges>
#include <stdexcept>
#include <system_error>

namespace arena::application {
namespace {

bool valid_key_text(const std::string_view key) {
    return key.size() == 64 &&
           std::ranges::all_of(key, [](const unsigned char character) {
               return std::isdigit(character) != 0 ||
                      (character >= 'a' && character <= 'f');
           });
}

std::string read_client_key(const std::filesystem::path& filename) {
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(filename, status_error);
    if (status_error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        throw std::runtime_error("client authentication key must be a regular non-symlink file");
    }

    std::ifstream input(filename, std::ios::binary);
    if (!input) {
        throw std::runtime_error("client authentication key cannot be read");
    }
    std::string key{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (!key.empty() && key.back() == '\n') {
        key.pop_back();
    }
    if (!key.empty() && key.back() == '\r') {
        key.pop_back();
    }
    if (!valid_key_text(key)) {
        throw std::runtime_error("client authentication key is invalid");
    }
    return key;
}

void require_secure_state_directory(const std::filesystem::path& directory) {
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(directory, status_error);
    if (status_error || !std::filesystem::is_directory(status) ||
        std::filesystem::is_symlink(status)) {
        throw std::runtime_error("client authentication state directory is unsafe");
    }
}

} // namespace

LocalAuth::LocalAuth(std::filesystem::path state_directory)
    : state_directory_(std::move(state_directory)),
      client_key_path_(state_directory_ / "client.key") {}

std::string LocalAuth::random_token() {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    std::array<unsigned char, 32> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    std::array<char, 65> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), bytes.data(), bytes.size());
    return encoded.data();
}

void LocalAuth::initialize() {
    std::scoped_lock lock(mutex_);
    if (initialized_) {
        return;
    }

    std::filesystem::create_directories(state_directory_);
    require_secure_state_directory(state_directory_);

    std::error_code key_status_error;
    const auto key_status =
        std::filesystem::symlink_status(client_key_path_, key_status_error);
    if (!key_status_error && key_status.type() != std::filesystem::file_type::not_found) {
        client_key_ = read_client_key(client_key_path_);
        initialized_ = true;
        return;
    }
    if (key_status_error &&
        key_status_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("client authentication key state cannot be inspected");
    }

    const std::string generated_key = random_token();
    const auto temporary = std::filesystem::path(
        client_key_path_.string() + "." + random_token() + ".tmp");
    try {
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error("cannot create client authentication key");
            }
            output << generated_key << '\n';
            output.flush();
            if (!output) {
                throw std::runtime_error("cannot persist client authentication key");
            }
        }
        std::filesystem::permissions(
            temporary,
            std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace);

        std::error_code publish_error;
        std::filesystem::create_hard_link(
            temporary,
            client_key_path_,
            publish_error);
        if (publish_error) {
            std::error_code destination_error;
            const auto destination_status = std::filesystem::symlink_status(
                client_key_path_, destination_error);
            if (destination_error ||
                destination_status.type() == std::filesystem::file_type::not_found) {
                throw std::runtime_error(
                    "cannot publish client authentication key: " +
                    publish_error.message());
            }
        }
        client_key_ = read_client_key(client_key_path_);
        initialized_ = true;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
    std::error_code cleanup_error;
    std::filesystem::remove(temporary, cleanup_error);
    if (cleanup_error) {
        throw std::runtime_error(
            "cannot remove temporary client authentication key: " +
            cleanup_error.message());
    }
}

const std::filesystem::path& LocalAuth::client_key_path() const {
    return client_key_path_;
}

bool LocalAuth::authenticate_bearer(const std::string& authorization) const {
    constexpr std::string_view prefix = "Bearer ";
    if (!authorization.starts_with(prefix)) {
        return false;
    }
    const std::string_view presented{
        authorization.data() + prefix.size(),
        authorization.size() - prefix.size(),
    };
    if (!valid_key_text(presented)) {
        return false;
    }

    const auto matches = [&presented](const std::string& key) {
        return valid_key_text(key) &&
               sodium_memcmp(presented.data(), key.data(), key.size()) == 0;
    };

    std::scoped_lock lock(mutex_);
    if (!initialized_) {
        return false;
    }
    if (matches(client_key_)) {
        return true;
    }

    try {
        client_key_ = read_client_key(client_key_path_);
    } catch (...) {
        return false;
    }
    return matches(client_key_);
}

void LocalAuth::prune_expired() {
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(pairing_tokens_, [now](const auto& item) {
        return item.second.expires_at <= now;
    });
    std::erase_if(sessions_, [now](const auto& item) {
        return item.second.expires_at <= now;
    });
}

std::string LocalAuth::issue_pairing_token() {
    std::scoped_lock lock(mutex_);
    if (!initialized_) {
        throw std::runtime_error("local authentication is not initialized");
    }
    prune_expired();
    const std::string token = random_token();
    pairing_tokens_.emplace(
        token,
        ExpiringValue{"", std::chrono::steady_clock::now() + std::chrono::seconds(60)});
    return token;
}

std::optional<BrowserSession> LocalAuth::exchange_pairing_token(
    const std::string& token) {
    std::scoped_lock lock(mutex_);
    if (!initialized_) {
        return std::nullopt;
    }
    prune_expired();
    const auto pairing = pairing_tokens_.find(token);
    if (pairing == pairing_tokens_.end()) {
        return std::nullopt;
    }
    pairing_tokens_.erase(pairing);
    BrowserSession session{random_token(), random_token()};
    sessions_.emplace(
        session.id,
        ExpiringValue{
            session.csrf_token,
            std::chrono::steady_clock::now() + std::chrono::hours(12),
        });
    return session;
}

std::optional<BrowserSession> LocalAuth::resume_browser_session(
    const std::string& session_id) const {
    std::scoped_lock lock(mutex_);
    if (!initialized_) {
        return std::nullopt;
    }
    const auto session = sessions_.find(session_id);
    if (session == sessions_.end() ||
        session->second.expires_at <= std::chrono::steady_clock::now()) {
        return std::nullopt;
    }
    return BrowserSession{session_id, session->second.value};
}

bool LocalAuth::authenticate_session(const std::string& session_id) const {
    std::scoped_lock lock(mutex_);
    if (!initialized_) {
        return false;
    }
    const auto session = sessions_.find(session_id);
    return session != sessions_.end() &&
           session->second.expires_at > std::chrono::steady_clock::now();
}

bool LocalAuth::validate_csrf(
    const std::string& session_id,
    const std::string& csrf_token) const {
    std::scoped_lock lock(mutex_);
    if (!initialized_) {
        return false;
    }
    const auto session = sessions_.find(session_id);
    return session != sessions_.end() &&
           session->second.expires_at > std::chrono::steady_clock::now() &&
           session->second.value.size() == csrf_token.size() &&
           sodium_memcmp(
               session->second.value.data(), csrf_token.data(), csrf_token.size()) == 0;
}

}  // namespace arena::application
