#include "arena/adapters/report_renderer.hpp"
#include "arena/application/local_auth.hpp"
#include "arena/domain/uuid.hpp"
#include "arena/platform/client_key_store.hpp"
#include "arena/platform/operator_credential_file.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace {

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("pairing tokens are one-time and browser sessions require CSRF") {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("arena-auth-" + arena::domain::uuid_v7());
    const auto key_store = arena::platform::make_client_key_store(directory);
    arena::application::LocalAuth auth(key_store);
    auth.initialize();

    const std::string key = key_store->load_existing().value;
    REQUIRE(auth.authenticate_bearer("Bearer " + key));
    REQUIRE_FALSE(auth.authenticate_bearer("Bearer wrong"));
    const std::string challenge(64, 'a');
    const std::string audience = "http://127.0.0.1:47831";
    const std::string proof = auth.server_proof(challenge, audience);
    REQUIRE(proof.size() == 64);
    REQUIRE(
        proof == arena::application::LocalAuth::compute_server_proof(
                     key, challenge, audience));
    REQUIRE(
        proof != arena::application::LocalAuth::compute_server_proof(
                     key, challenge, "http://127.0.0.1:47832"));
    REQUIRE_THROWS_AS(
        auth.server_proof("not-a-challenge", audience),
        std::invalid_argument);
    REQUIRE_THROWS_AS(
        auth.server_proof(challenge, "http://localhost:47831"),
        std::invalid_argument);
    const std::string rotated_key(64, 'a');
    std::ofstream(directory / "client.key", std::ios::binary | std::ios::trunc)
        << rotated_key << '\n';
    REQUIRE(
        auth.server_proof(challenge, audience) ==
        arena::application::LocalAuth::compute_server_proof(
            rotated_key, challenge, audience));
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

    std::filesystem::remove_all(directory);
}

TEST_CASE("operator credential files commit atomically and roll back cleanly") {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("arena-operator-credential-" + arena::domain::uuid_v7());
    const auto destination = directory / "credentials" / "lichess-bot.token";

    {
        arena::platform::OperatorCredentialFileUpdate update(
            destination, "synthetic-first-value");
        REQUIRE(read_text(destination) == "synthetic-first-value");
    }
    REQUIRE_FALSE(std::filesystem::exists(destination));

    {
        arena::platform::OperatorCredentialFileUpdate update(
            destination, "synthetic-authoritative-value");
        update.commit();
    }
    REQUIRE(read_text(destination) == "synthetic-authoritative-value");

    {
        arena::platform::OperatorCredentialFileUpdate update(
            destination, "synthetic-rejected-value");
        REQUIRE(read_text(destination) == "synthetic-rejected-value");
    }
    REQUIRE(read_text(destination) == "synthetic-authoritative-value");

    {
        arena::platform::OperatorCredentialFileUpdate update(
            destination, "synthetic-replacement-value");
        update.commit();
    }
    REQUIRE(read_text(destination) == "synthetic-replacement-value");
    for (const auto& entry :
         std::filesystem::directory_iterator(destination.parent_path())) {
        REQUIRE(entry.path() == destination);
    }
#ifndef _WIN32
    const auto permissions = std::filesystem::status(destination).permissions();
    REQUIRE(
        (permissions &
         (std::filesystem::perms::group_all |
          std::filesystem::perms::others_all)) == std::filesystem::perms::none);
#endif

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
