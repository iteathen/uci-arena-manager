#include "arena/application/local_auth.hpp"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>
#include <stdexcept>
#include <string_view>

namespace arena::application {
namespace {

bool lowercase_hex_token(const std::string_view value) {
    return value.size() == 64 &&
           std::ranges::all_of(value, [](const unsigned char character) {
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

}  // namespace

LocalAuth::LocalAuth(
    std::shared_ptr<ports::ClientKeyStore> client_key_store)
    : client_key_store_(std::move(client_key_store)) {
    if (!client_key_store_) {
        throw std::invalid_argument("LocalAuth requires a client key store");
    }
}

LocalAuth::~LocalAuth() {
    std::scoped_lock lock(mutex_);
    if (!client_key_.empty()) {
        sodium_memzero(client_key_.data(), client_key_.size());
    }
}

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
    try {
        adopt_client_key(client_key_store_->load_existing());
        return;
    } catch (const ports::ClientKeyFailure& error) {
        if (error.kind() != ports::ClientKeyFailureKind::missing) throw;
    }
    std::string proposed = random_token();
    try {
        ports::ClientKeyMaterial material =
            client_key_store_->load_or_create(proposed);
        sodium_memzero(proposed.data(), proposed.size());
        adopt_client_key(std::move(material));
    } catch (...) {
        sodium_memzero(proposed.data(), proposed.size());
        throw;
    }
}

void LocalAuth::adopt_client_key(ports::ClientKeyMaterial material) const {
    if (!lowercase_hex_token(material.value) ||
        material.object_identity.empty()) {
        throw ports::ClientKeyFailure(ports::ClientKeyFailureKind::invalid);
    }
    if (!client_key_.empty()) {
        sodium_memzero(client_key_.data(), client_key_.size());
    }
    client_key_ = std::move(material.value);
    client_key_identity_ = std::move(material.object_identity);
}

std::string LocalAuth::compute_server_proof(
    const std::string& client_key,
    const std::string& challenge,
    const std::string& audience) {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
    if (!lowercase_hex_token(client_key) || !lowercase_hex_token(challenge) ||
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
    adopt_client_key(client_key_store_->load_existing());
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
    const auto matches = [&presented](const std::string& key) {
        return presented.size() == key.size() &&
               sodium_memcmp(presented.data(), key.data(), key.size()) == 0;
    };

    std::scoped_lock lock(mutex_);
    if (matches(client_key_)) {
        return true;
    }

    try {
        adopt_client_key(client_key_store_->load_existing());
    } catch (const std::exception&) {
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
    prune_expired();
    const std::string token = random_token();
    pairing_tokens_.emplace(
        token,
        ExpiringValue{"", std::chrono::steady_clock::now() + std::chrono::seconds(60)});
    return token;
}

std::optional<BrowserSession> LocalAuth::exchange_pairing_token(const std::string& token) {
    std::scoped_lock lock(mutex_);
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
    const auto session = sessions_.find(session_id);
    if (session == sessions_.end() ||
        session->second.expires_at <= std::chrono::steady_clock::now()) {
        return std::nullopt;
    }
    return BrowserSession{session_id, session->second.value};
}

bool LocalAuth::authenticate_session(const std::string& session_id) const {
    std::scoped_lock lock(mutex_);
    const auto session = sessions_.find(session_id);
    return session != sessions_.end() &&
           session->second.expires_at > std::chrono::steady_clock::now();
}

bool LocalAuth::validate_csrf(
    const std::string& session_id,
    const std::string& csrf_token) const {
    std::scoped_lock lock(mutex_);
    const auto session = sessions_.find(session_id);
    return session != sessions_.end() &&
           session->second.expires_at > std::chrono::steady_clock::now() &&
           session->second.value.size() == csrf_token.size() &&
           sodium_memcmp(
               session->second.value.data(), csrf_token.data(), csrf_token.size()) == 0;
}

}  // namespace arena::application
