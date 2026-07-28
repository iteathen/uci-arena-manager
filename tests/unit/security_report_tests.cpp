#include "arena/adapters/report_renderer.hpp"
#include "arena/application/local_auth.hpp"
#include "arena/domain/uuid.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE("pairing tokens are one-time and browser sessions require CSRF") {
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("arena-auth-" + arena::domain::uuid_v7());
    arena::application::LocalAuth auth(directory);
    auth.initialize();

    std::ifstream input(auth.client_key_path());
    std::string key;
    std::getline(input, key);
    input.close();
    REQUIRE(auth.authenticate_bearer("Bearer " + key));
    REQUIRE_FALSE(auth.authenticate_bearer("Bearer wrong"));

    const std::string token = auth.issue_pairing_token();
    const auto session = auth.exchange_pairing_token(token);
    REQUIRE(session);
    REQUIRE_FALSE(auth.exchange_pairing_token(token));
    REQUIRE(auth.authenticate_session(session->id));
    REQUIRE(auth.validate_csrf(session->id, session->csrf_token));
    REQUIRE_FALSE(auth.validate_csrf(session->id, "wrong"));

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
