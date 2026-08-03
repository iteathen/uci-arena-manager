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

bool numeric_loopback_audience(const std::string_view value) {
    constexpr std::string_view prefix = "http://127.0.0.1:";
    if (!value.starts_with(prefix)) {
        return false;
    }
    const std::string_view port = value.substr(prefix.size());
    if (port.empty() || port.size() > 5) {
        return false;
    }
    unsigned number = 0;
    for (const unsigned char character : port) {
        if (std::isdigit(character) == 0) {
            return false;
        }
        number = number * 10 + static_cast<unsigned>(character - '0');
    }
    return number != 0 && number <= 65'535;
}

std::string read_client_key(const std::filesystem::path& filename) {
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(filename, status_error);
    if (status_error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status)) {
        throw std::runtime_error(
            "client authentication key must be a regular non-symlink file");
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

void restrict_key_permissions(const std::filesystem::path& filename) {
    std::filesystem::permissions(
        filename,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
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
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }

    std::filesystem::create_directories(state_directory_);
    require_secure_state_directory(state_directory_);

    std::error_code key_status_error;
    const auto key_status =
        std::filesystem::symlink_status(client_key_path_, key_status_error);
    if (!key_status_error &&
        key_status.type() != std::filesystem::file_type::not_found) {
        const std::string persisted_key = read_client_key(client_key_path_);
        restrict_key_permissions(client_key_path_);
        client_key_ = persisted_key;
        initialized_ = true;
        return;
    }
    if (key_status_error &&
        key_status_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error(
            "client authentication key state cannot be inspected");
    }

    const std::string generated_key = random_token();
    const auto temporary = std::filesystem::path(
        client_key_path_.string() + "." + random_token() + ".tmp");
    std::string selected_key;
    try {
        {
            std::ofstream output(
                temporary,
                std::ios::binary | std::ios::trunc);
            if (!output) {
                throw std::runtime_error(
                    "cannot create client authentication key");
            }
            output << generated_key << '\n';
            output.flush();
            if (!output) {
                throw std::runtime_error(
                    "cannot persist client authentication key");
            }
        }
        restrict_key_permissions(temporary);

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
                destination_status.type() ==
                    std::filesystem::file_type::not_found) {
                throw std::runtime_error(
                    "cannot publish client authentication key: " +
                    publish_error.message());
            }
        }
        selected_key = read_client_key(client_key_path_);
        restrict_key_permissions(client_key_path_);

        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        if (cleanup_error) {
            throw std::runtime_error(
                "cannot remove temporary client authentication key: " +
                cleanup_error.message());
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }

    client_key_ = std::move(selected_key);
    initialized_ = true;
}

const std::filesystem::path& LocalAuth::client_key_path() const {
    return client_key_path_;
}

std::string LocalAuth::compute_server_proof(
    const std::string& client_key,
    const std::string& challenge,
    const std::string& audience) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    if (!valid_key_text(client_key) || !valid_key_text(challenge) ||
        !numeric_loopback_audience(audience)) {
        throw std::invalid_argument(
            "server proof requires lowercase hexadecimal key and challenge values and a numeric loopback audience");
    }
    std::array<unsigned char, crypto_auth_hmacsha256_KEYBYTES> key{};
    std::size_t key_size = 0;
    if (sodium_hex2bin(
            key.data(), key.size(), client_key.data(), client_key.size(),
            nullptr, &key_size, nullptr) != 0 || key_size != key.size()) {
        throw std::invalid_argument("server proof hexadecimal input is invalid");
    }
    const std::string message =
        "uci-arena-manager-server-proof-v1\n" + challenge + "\n" + audience;
    std::array<unsigned char, crypto_auth_hmacsha256_BYTES> proof{};
    crypto_auth_hmacsha256(
        proof.data(),
        reinterpret_cast<const unsigned char*>(message.data()),
        message.size(),
        key.data());
    std::array<char, crypto_auth_hmacsha256_BYTES * 2 + 1> encoded{};
    sodium_bin2hex(encoded.data(), encoded.size(), proof.data(), proof.size());
    return encoded.data();
}

std::string LocalAuth::server_proof(
    const std::string& challenge,
    const std::string& audience) const {
    std::scoped_lock lock(mutex_);
    if (!initialized_) {
        throw std::runtime_error("local authentication is not initialized");
    }
    client_key_ = read_client_key(client_key_path_);
    return compute_server_proof(client_key_, challenge, audience);
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
               sodium_memcmp(
                   presented.data(), key.data(), key.size()) == 0;
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
        throw std::runtime_error(
            "local authentication is not initialized");
    }
    prune_expired();
    const std::string token = random_token();
    pairing_tokens_.emplace(
        token,
        ExpiringValue{
            "",
            std::chrono::steady_clock::now() +
                std::chrono::seconds(60),
        });
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
            std::chrono::steady_clock::now() +
                std::chrono::hours(12),
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
        session->second.expires_at <=
            std::chrono::steady_clock::now()) {
        return std::nullopt;
    }
    return BrowserSession{session_id, session->second.value};
}

bool LocalAuth::authenticate_session(
    const std::string& session_id) const {
    std::scoped_lock lock(mutex_);
    if (!initialized_) {
        return false;
    }
    const auto session = sessions_.find(session_id);
    return session != sessions_.end() &&
           session->second.expires_at >
               std::chrono::steady_clock::now();
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
           session->second.expires_at >
               std::chrono::steady_clock::now() &&
           session->second.value.size() == csrf_token.size() &&
           sodium_memcmp(
               session->second.value.data(),
               csrf_token.data(),
               csrf_token.size()) == 0;
}

}  // namespace arena::application
