#include "arena/adapters/report_renderer.hpp"
#include "arena/application/local_auth.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path unique_directory(const std::string& prefix) {
    return std::filesystem::temp_directory_path() /
           (prefix + arena::domain::uuid_v7());
}

} // namespace

TEST_CASE("local bearer authentication fails closed before initialization") {
    const auto directory = unique_directory("arena-auth-uninitialized-");
    arena::application::LocalAuth auth(directory);

    REQUIRE_FALSE(auth.authenticate_bearer("Bearer "));
    REQUIRE_FALSE(auth.authenticate_bearer("Bearer " + std::string(64, 'a')));
    REQUIRE_FALSE(auth.authenticate_session(""));
    REQUIRE_FALSE(auth.validate_csrf("", ""));
    REQUIRE_FALSE(auth.exchange_pairing_token(""));
    REQUIRE_THROWS(auth.issue_pairing_token());
}

TEST_CASE("local authentication rejects malformed existing key state") {
    const auto directory = unique_directory("arena-auth-malformed-");
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "client.key", std::ios::binary | std::ios::trunc)
        << std::string(64, 'z') << '\n';

    arena::application::LocalAuth auth(directory);
    REQUIRE_THROWS(auth.initialize());

    std::filesystem::remove_all(directory);
}

TEST_CASE("local authentication rejects a symlinked client key") {
    const auto directory = unique_directory("arena-auth-symlink-");
    const auto outside = unique_directory("arena-auth-outside-");
    std::filesystem::create_directories(directory);
    std::filesystem::create_directories(outside.parent_path());
    std::ofstream(outside, std::ios::binary | std::ios::trunc)
        << std::string(64, 'a') << '\n';
    std::error_code link_error;
    std::filesystem::create_symlink(outside, directory / "client.key", link_error);
    if (!link_error) {
        arena::application::LocalAuth auth(directory);
        REQUIRE_THROWS(auth.initialize());
    }

    std::filesystem::remove_all(directory);
    std::filesystem::remove(outside);
}

TEST_CASE("pairing tokens are one-time and browser sessions require CSRF") {
    const auto directory = unique_directory("arena-auth-");
    arena::application::LocalAuth auth(directory);
    auth.initialize();

    std::ifstream input(auth.client_key_path());
    std::string key;
    std::getline(input, key);
    input.close();
    REQUIRE(auth.authenticate_bearer("Bearer " + key));
    REQUIRE_FALSE(auth.authenticate_bearer("Bearer wrong"));
    const std::string rotated_key(64, 'a');
    std::ofstream(auth.client_key_path(), std::ios::binary | std::ios::trunc)
        << rotated_key << '\n';
    REQUIRE(auth.authenticate_bearer("Bearer " + rotated_key));
    REQUIRE_FALSE(auth.authenticate_bearer("Bearer " + key));

    const std::string token = auth.issue_pairing_token();
    const auto session = auth.exchange_pairing_token(token);
    REQUIRE(session);
    REQUIRE_FALSE(auth.exchange_pairing_token(token));
    REQUIRE(auth.authenticate_session(session->id));
    const auto resumed = auth.resume_browser_session(session->id);
    REQUIRE(resumed);
    REQUIRE(resumed->id == session->id);
    REQUIRE(resumed->csrf_token == session->csrf_token);
    REQUIRE_FALSE(auth.resume_browser_session("missing"));
    REQUIRE(auth.validate_csrf(session->id, session->csrf_token));
    REQUIRE_FALSE(auth.validate_csrf(session->id, "wrong"));

    std::size_t key_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().starts_with("client.key")) {
            ++key_files;
        }
    }
    REQUIRE(key_files == 1);

    std::filesystem::remove_all(directory);
}

TEST_CASE("report renderers preserve one document across native formats") {
    const nlohmann::json document{
        {"title", "LC0 < Stockfish evidence"},
        {"schema_version", 1},
        {"pgn", "[Event \"Local match\"]\n[Result \"1/2-1/2\"]\n\n1. e4 e5 1/2-1/2\n"},
    };
    REQUIRE(arena::adapters::ReportRenderer::json(document).find("schema_version") !=
            std::string::npos);
    REQUIRE(arena::adapters::ReportRenderer::csv(document).starts_with("key,value"));
    REQUIRE(arena::adapters::ReportRenderer::html(document).find("&lt;") !=
            std::string::npos);
    REQUIRE(arena::adapters::ReportRenderer::pdf(document).starts_with("%PDF-"));
    REQUIRE(arena::adapters::ReportRenderer::pgn(document).find("Local match") !=
            std::string::npos);
}
