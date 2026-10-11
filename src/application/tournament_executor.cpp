#include "arena/application/tournament_executor.hpp"

#include "arena/adapters/uci_game_runner.hpp"
#include "arena/adapters/uci_process.hpp"
#include "arena/application/engine_onboarding.hpp"
#include "arena/application/engine_option_values.hpp"
#include "arena/domain/uuid.hpp"

#include <chess.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace arena::application {
namespace {

using nlohmann::json;

constexpr std::string_view standard_starting_fen =
    "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

std::string chess960_starting_fen(std::uint64_t index) {
    std::uint32_t position = static_cast<std::uint32_t>(index % 960U);
    std::array<char, 8> rank{};
    rank.fill(' ');
    rank[1U + 2U * (position % 4U)] = 'B';
    position /= 4U;
    rank[2U * (position % 4U)] = 'B';
    position /= 4U;
    std::vector<std::size_t> empty;
    for (std::size_t file = 0; file < rank.size(); ++file) {
        if (rank[file] == ' ') empty.push_back(file);
    }
    rank[empty[position % 6U]] = 'Q';
    position /= 6U;
    empty.clear();
    for (std::size_t file = 0; file < rank.size(); ++file) {
        if (rank[file] == ' ') empty.push_back(file);
    }
    constexpr std::array<std::array<std::size_t, 2>, 10> knight_pairs{{
        {{0, 1}}, {{0, 2}}, {{0, 3}}, {{0, 4}}, {{1, 2}},
        {{1, 3}}, {{1, 4}}, {{2, 3}}, {{2, 4}}, {{3, 4}},
    }};
    const auto knights = knight_pairs.at(position % 10U);
    rank[empty[knights[0]]] = 'N';
    rank[empty[knights[1]]] = 'N';
    empty.clear();
    for (std::size_t file = 0; file < rank.size(); ++file) {
        if (rank[file] == ' ') empty.push_back(file);
    }
    rank[empty[0]] = 'R';
    rank[empty[1]] = 'K';
    rank[empty[2]] = 'R';

    std::string white(rank.begin(), rank.end());
    std::string black = white;
    std::transform(
        black.begin(), black.end(), black.begin(),
        [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    std::string castling;
    castling.push_back(static_cast<char>('A' + empty[2]));
    castling.push_back(static_cast<char>('A' + empty[0]));
    castling.push_back(static_cast<char>('a' + empty[2]));
    castling.push_back(static_cast<char>('a' + empty[0]));
    return black + "/pppppppp/8/8/8/8/PPPPPPPP/" + white +
           " w " + castling + " - 0 1";
}

json load_opening_suite(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    const auto size = std::filesystem::file_size(path, error);
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::is_symlink(status) || size == 0 ||
        size > 16ULL * 1024ULL * 1024ULL) {
        throw std::runtime_error(
            "opening suite is unavailable or exceeds the 16 MiB boundary");
    }
    std::ifstream input(path, std::ios::binary);
    json suite;
    input >> suite;
    if (suite.value("schema", "") != "arena_opening_suite_v1" ||
        !suite.contains("positions") || !suite.at("positions").is_array() ||
        suite.at("positions").empty() || suite.at("positions").size() > 10'000) {
        throw std::runtime_error("opening suite schema or position count is invalid");
    }
    return suite.at("positions");
}

struct StartingPosition {
    std::string fen;
    bool chess960{};
    adapters::UciGameState state;
};

StartingPosition starting_position(
    const json& competition,
    const json& pairing) {
    const std::string competition_variant =
        competition.value("variant", "standard");
    const std::uint64_t opening_index = pairing.value("opening_index", 0ULL);
    StartingPosition result{
        competition_variant == "chess960"
            ? chess960_starting_fen(opening_index)
            : std::string(standard_starting_fen),
        competition_variant == "chess960",
        {},
    };
    if (competition.contains("resolved_opening_positions")) {
        const json& positions = competition.at("resolved_opening_positions");
        const json& selected = positions.at(
            static_cast<std::size_t>(opening_index % positions.size()));
        if (!selected.is_object() ||
            !selected.contains("starting_fen") ||
            !selected.at("starting_fen").is_string()) {
            throw std::runtime_error("opening suite position is malformed");
        }
        const std::string variant = selected.value("variant", "standard");
        if (variant != competition_variant) {
            throw std::runtime_error(
                "opening suite position variant does not match the competition");
        }
        result.fen = selected.at("starting_fen").get<std::string>();
        result.chess960 = variant == "chess960";
        result.state.uci_moves =
            selected.value("uci_moves", std::vector<std::string>{});
    }
    chess::Board board;
    if (!(result.chess960 ? board.setXfen(result.fen) : board.setFen(result.fen))) {
        throw std::runtime_error("opening suite starting_fen is invalid");
    }
    for (const auto& move_text : result.state.uci_moves) {
        const chess::Move move = chess::uci::uciToMove(board, move_text);
        if (move == chess::Move::NO_MOVE || !board.isLegal(move)) {
            throw std::runtime_error(
                "opening suite contains an illegal UCI move");
        }
        result.state.san_moves.push_back(chess::uci::moveToSan(board, move));
        board.makeMove(move);
        result.state.per_ply_fen.push_back(board.getFen());
        result.state.evaluations_cp.push_back(std::nullopt);
        result.state.principal_variations.emplace_back();
    }
    const std::uint64_t initial = competition.at("time_control")
                                      .value("initial_ms", 60'000ULL);
    result.state.white_time_ms = initial;
    result.state.black_time_ms = initial;
    return result;
}

std::string run_key(const std::string& kind, const std::string& id) {
    return kind + ":" + id;
}

ports::CommandRecord system_command(
    const std::string& action,
    const std::string& timestamp,
    const std::string& command_id) {
    return {
        command_id,
        domain::uuid_v7(),
        "runtime-" + domain::uuid_v7(),
        action,
        "completed",
        json{{"status", "completed"}, {"command_id", command_id}}.dump(),
        timestamp,
    };
}

ports::EventRecord system_event(
    const ports::ResourceRecord& resource,
    const std::string& event_kind,
    const std::string& timestamp,
    const std::string& command_id) {
    return {
        0,
        domain::uuid_v7(),
        timestamp,
        "state",
        event_kind,
        resource.kind,
        resource.id,
        resource.revision,
        command_id,
        json{
            {"actor", "manager-runtime"},
            {"resource_id", resource.id},
            {"revision", resource.revision},
        }.dump(),
    };
}

void update_resource_data(
    const std::shared_ptr<ports::Store>& store,
    const std::string& kind,
    const std::string& id,
    const std::string& action,
    const std::string& event_kind,
    const std::function<void(json&)>& mutate) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        const auto existing = store->get_resource(kind, id);
        if (!existing) {
            throw std::runtime_error(kind + " resource disappeared during execution");
        }
        ports::ResourceRecord updated = *existing;
        json data = json::parse(existing->data_json);
        mutate(data);
        const std::string now = domain::utc_now();
        updated.revision = existing->revision + 1;
        updated.updated_at = now;
        updated.data_json = data.dump();
        const std::string command_id = domain::uuid_v7();
        try {
            store->update_resource(
                updated,
                existing->revision,
                system_command(action, now, command_id),
                system_event(updated, event_kind, now, command_id));
            return;
        } catch (const std::exception& error) {
            if (std::string_view(error.what()) != "resource revision conflict" ||
                attempt == 4) {
                throw;
            }
        }
    }
}

json engine_data(
    const std::shared_ptr<ports::Store>& store,
    const std::string& id) {
    const auto resource = store->get_resource("engine", id);
    if (!resource) {
        throw std::runtime_error("scheduled engine is no longer registered: " + id);
    }
    return json::parse(resource->data_json);
}

std::map<std::string, std::string> string_map(const json& value) {
    return normalize_engine_option_values(value);
}

void add_receipt_default_model_option(
    const json& data,
    std::map<std::string, std::string>& options) {
    if (options.contains("ModelRoot") ||
        data.value("onboarding_source", "") != "install_receipt" ||
        !data.contains("default_model") ||
        !data.at("default_model").is_object()) {
        return;
    }
    const json& model = data.at("default_model");
    if (!model.contains("root") || !model.at("root").is_string()) {
        return;
    }
    const std::filesystem::path relative_root =
        model.at("root").get<std::string>();
    if (relative_root.empty() || relative_root.is_absolute() ||
        std::ranges::any_of(
            relative_root,
            [](const std::filesystem::path& part) { return part == ".."; })) {
        return;
    }
    const std::filesystem::path working_directory =
        data.value("working_directory", "");
    std::error_code error;
    const std::filesystem::path model_root =
        std::filesystem::weakly_canonical(
            working_directory / relative_root,
            error);
    if (error || !std::filesystem::is_directory(model_root, error) || error ||
        std::filesystem::is_symlink(model_root, error) || error) {
        return;
    }
    options.emplace("ModelRoot", model_root.string());
}

domain::TimeControl original_clock_control(const json& competition) {
    const json& time = competition.at("time_control");
    if (!time.is_object() || !time.contains("initial_ms") || !time.contains("increment_ms") ||
        !time.at("initial_ms").is_number_integer() || !time.at("increment_ms").is_number_integer() ||
        time.at("initial_ms") <= 0 || time.at("increment_ms") < 0 ||
        time.at("initial_ms") > 9007199254740991ULL || time.at("increment_ms") > 9007199254740991ULL) {
        throw std::runtime_error("competition original clock is missing or invalid");
    }
    domain::TimeControl actual;
    actual.initial_ms = time.at("initial_ms").get<std::uint64_t>();
    actual.increment_ms = time.at("increment_ms").get<std::uint64_t>();
    return actual;
}

adapters::UciGameEngine game_engine(
    const std::shared_ptr<ports::Store>& store,
    const json& competition,
    const std::string& id,
    const TournamentExecutor::ComponentInvocationGate& component_invocation_gate) {
    const json data = engine_data(store, id);
    if (data.value("onboarding_source", "") == "install_receipt") {
        const std::string component_id = data.value("component_id", "");
        if (!component_id.empty() && !component_invocation_gate) {
            throw std::runtime_error(
                "receipt-managed engine has no component verification authority");
        }
        if (!component_id.empty()) {
            component_invocation_gate(component_id);
        }
    }
    auto options =
        string_map(data.value("option_overrides", json::object()));
    add_receipt_default_model_option(data, options);
    adapters::UciGameEngine engine{
        id,
        data.at("fingerprint").get<std::string>(),
        data.at("executable").get<std::string>(),
        data.at("working_directory").get<std::string>(),
        data.value("arguments", std::vector<std::string>{}),
        std::move(options),
    };
    const json overrides = competition.value(
        "engine_option_overrides", json::object());
    if (overrides.contains(id)) {
        for (const auto& [name, value] : string_map(overrides.at(id))) {
            engine.options[name] = value;
        }
    }
    if (data.contains("clock_option_bindings")) {
        engine.clock_bindings = EngineOnboarding::clock_option_bindings(data.at("clock_option_bindings"),
            data.value("option_overrides", json::object()));
        engine.options = domain::bind_clock_options(engine.options, engine.clock_bindings, original_clock_control(competition));
    }
    return engine;
}

void validate_competition_engines(
    const std::shared_ptr<ports::Store>& store,
    const json& competition,
    const TournamentExecutor::ComponentInvocationGate& component_invocation_gate) {
    std::set<std::string> engine_ids;
    for (const auto& engine_id : competition.at("engine_ids")) {
        if (!engine_id.is_string()) {
            throw std::runtime_error(
                "competition contains a malformed engine identity");
        }
        engine_ids.insert(engine_id.get<std::string>());
    }

    std::vector<std::pair<json, adapters::UciGameEngine>> prepared;
    for (const auto& engine_id : engine_ids) {
        prepared.emplace_back(engine_data(store, engine_id),
            game_engine(store, competition, engine_id, component_invocation_gate));
    }
    for (const auto& [registered, engine] : prepared) {
        const std::string& engine_id = engine.resource_id;
        json expected_runtime = nullptr;
        if (registered.contains("launch_profile") &&
            registered.at("launch_profile").is_object() &&
            registered.at("launch_profile").value("state", "") ==
                "qualified_timing") {
            const json& launch_profile = registered.at("launch_profile");
            expected_runtime = launch_profile.contains("expected_runtime")
                ? launch_profile.at("expected_runtime")
                : json(nullptr);
        }
        const adapters::UciProbeResult probe = adapters::UciProcess::probe(
            engine.executable,
            engine.working_directory,
            engine.arguments,
            adapters::UciProbeTimeouts{},
            engine.options,
            !expected_runtime.is_null(),
            engine.clock_bindings.empty() ? std::nullopt : std::optional{original_clock_control(competition)},
            engine.clock_bindings);
        if (!probe.ok) {
            throw std::runtime_error(
                "engine preflight failed before game admission for " +
                engine_id + ": " + probe.error);
        }
        if (probe.fingerprint != engine.fingerprint) {
            throw std::runtime_error(
                "registered engine identity changed before game admission for " +
                engine_id);
        }
        if (!expected_runtime.is_null()) {
            if (!probe.runtime_status.has_value()) {
                throw std::runtime_error(
                    "qualified engine runtime status is unavailable before game admission for " +
                    engine_id);
            }
            if (const auto mismatch =
                    EngineOnboarding::runtime_identity_mismatch(
                        expected_runtime, *probe.runtime_status)) {
                throw std::runtime_error(
                    "qualified engine runtime identity changed before game admission for " +
                    engine_id + ": " + *mismatch);
            }
        }
    }
}

std::optional<ports::ResourceRecord> existing_game(
    const std::shared_ptr<ports::Store>& store,
    const std::string& competition_id,
    const std::uint32_t sequence) {
    for (const auto& record : store->list_resources("game", 10'000, 0)) {
        const json data = json::parse(record.data_json);
        if (data.value("competition_id", "") == competition_id &&
            data.value("sequence", 0U) == sequence) {
            return record;
        }
    }
    return std::nullopt;
}

ports::ResourceRecord create_game(
    const std::shared_ptr<ports::Store>& store,
    const std::string& competition_kind,
    const std::string& competition_id,
    const json& competition,
    const json& pairing,
    const adapters::UciGameEngine& white,
    const adapters::UciGameEngine& black) {
    const std::string now = domain::utc_now();
    const std::string id = domain::uuid_v7();
    const StartingPosition position = starting_position(competition, pairing);
    const json data{
        {"display_name",
         competition.value("display_name", "Competition") + " · game " +
             std::to_string(pairing.at("sequence").get<std::uint32_t>())},
        {"competition_kind", competition_kind},
        {"competition_id", competition_id},
        {"sequence", pairing.at("sequence")},
        {"round", pairing.at("round")},
        {"board", pairing.at("board")},
        {"starting_fen", position.fen},
        {"variant", competition.value("variant", "standard")},
        {"opening_plies", position.state.uci_moves.size()},
        {"uci_moves", position.state.uci_moves},
        {"san_moves", position.state.san_moves},
        {"per_ply_fen", position.state.per_ply_fen},
        {"evaluations_cp", position.state.evaluations_cp},
        {"principal_variations", position.state.principal_variations},
        {"clocks",
         {{"white_ms", position.state.white_time_ms},
          {"black_ms", position.state.black_time_ms}}},
        {"result", "*"},
        {"termination", "in_progress"},
        {"lifecycle", "running"},
        {"white_engine",
         {{"id", white.resource_id}, {"fingerprint", white.fingerprint},
          {"options", white.options}}},
        {"black_engine",
         {{"id", black.resource_id}, {"fingerprint", black.fingerprint},
          {"options", black.options}}},
    };
    ports::ResourceRecord resource{
        id,
        "game",
        "local",
        1,
        now,
        now,
        json{{"competition_id", competition_id}}.dump(),
        data.dump(),
    };
    const std::string command_id = domain::uuid_v7();
    store->create_resource(
        resource,
        system_command("game.runtime.create", now, command_id),
        system_event(resource, "game.started", now, command_id));
    return resource;
}

adapters::UciGameState persisted_game_state(const json& data) {
    const json clocks = data.value("clocks", json::object());
    return {
        data.value("uci_moves", std::vector<std::string>{}),
        data.value("san_moves", std::vector<std::string>{}),
        data.value("per_ply_fen", std::vector<std::string>{}),
        data.value(
            "evaluations_cp",
            std::vector<std::optional<std::int32_t>>(
                data.value("uci_moves", json::array()).size(), std::nullopt)),
        data.value(
            "principal_variations",
            std::vector<std::vector<std::string>>(
                data.value("uci_moves", json::array()).size())),
        clocks.value("white_ms", 0ULL),
        clocks.value("black_ms", 0ULL),
    };
}

void persist_ply(
    const std::shared_ptr<ports::Store>& store,
    const std::string& game_id,
    const adapters::UciGameState& state) {
    update_resource_data(
        store,
        "game",
        game_id,
        "game.runtime.commit_ply",
        "game.ply_committed",
        [&state](json& data) {
            data["uci_moves"] = state.uci_moves;
            data["san_moves"] = state.san_moves;
            data["per_ply_fen"] = state.per_ply_fen;
            data["evaluations_cp"] = state.evaluations_cp;
            data["principal_variations"] = state.principal_variations;
            data["clocks"] = {
                {"white_ms", state.white_time_ms},
                {"black_ms", state.black_time_ms},
            };
            data["last_committed_ply"] = state.uci_moves.size();
        });
}

void persist_game_result(
    const std::shared_ptr<ports::Store>& store,
    const std::string& game_id,
    const adapters::UciGameResult& result,
    const bool operator_cancelled) {
    update_resource_data(
        store,
        "game",
        game_id,
        "game.runtime.finish",
        result.lifecycle == "completed" ? "game.completed" :
        operator_cancelled ? "game.cancelled" : "game.interrupted",
        [&result, operator_cancelled](json& data) {
            data["uci_moves"] = result.state.uci_moves;
            data["san_moves"] = result.state.san_moves;
            data["per_ply_fen"] = result.state.per_ply_fen;
            data["evaluations_cp"] = result.state.evaluations_cp;
            data["principal_variations"] =
                result.state.principal_variations;
            data["clocks"] = {
                {"white_ms", result.state.white_time_ms},
                {"black_ms", result.state.black_time_ms},
            };
            data["result"] = result.result;
            data["termination"] =
                result.lifecycle == "cancelled" && !operator_cancelled
                    ? "manager_interrupted"
                    : result.termination;
            data["lifecycle"] =
                result.lifecycle == "cancelled" && !operator_cancelled
                    ? "interrupted"
                    : result.lifecycle;
            if (!result.detail.empty()) {
                data["failure_detail"] = result.detail;
            } else {
                data.erase("failure_detail");
            }
        });
}

json competition_data(
    const std::shared_ptr<ports::Store>& store,
    const std::string& kind,
    const std::string& id) {
    const auto resource = store->get_resource(kind, id);
    if (!resource) {
        throw std::runtime_error("competition resource does not exist");
    }
    return json::parse(resource->data_json);
}

std::vector<json> competition_games(
    const std::shared_ptr<ports::Store>& store,
    const std::string& competition_id) {
    std::vector<json> games;
    for (const auto& record : store->list_resources("game", 10'000, 0)) {
        json game = json::parse(record.data_json);
        if (game.value("competition_id", "") == competition_id) {
            games.push_back(std::move(game));
        }
    }
    std::sort(
        games.begin(), games.end(),
        [](const json& left, const json& right) {
            return left.value("sequence", 0U) < right.value("sequence", 0U);
        });
    return games;
}

std::optional<std::string> persist_sprt_state(
    const std::shared_ptr<ports::Store>& store,
    const std::string& kind,
    const std::string& id) {
    const json competition = competition_data(store, kind, id);
    if (competition.value("format", "") != "sprt") {
        return std::nullopt;
    }
    const json existing = competition.at("sprt");
    if (existing.contains("verdict") && !existing.at("verdict").is_null()) {
        return existing.at("verdict").get<std::string>();
    }
    const std::string reference = competition.at("engine_ids").at(0);
    std::size_t games = 0;
    double score = 0.0;
    for (const auto& game : competition_games(store, id)) {
        if (game.value("lifecycle", "") != "completed") continue;
        const std::string result = game.value("result", "*");
        const bool reference_white =
            game.at("white_engine").value("id", "") == reference;
        if (result == "1-0") {
            score += reference_white ? 1.0 : 0.0;
        } else if (result == "0-1") {
            score += reference_white ? 0.0 : 1.0;
        } else if (result == "1/2-1/2") {
            score += 0.5;
        } else {
            continue;
        }
        ++games;
    }
    const double elo0 = existing.value("elo0", 0.0);
    const double elo1 = existing.value("elo1", 5.0);
    const double alpha = existing.value("alpha", 0.05);
    const double beta = existing.value("beta", 0.05);
    const auto expectation = [](const double elo) {
        return 1.0 / (1.0 + std::pow(10.0, -elo / 400.0));
    };
    const double p0 = expectation(elo0);
    const double p1 = expectation(elo1);
    const double llr =
        score * std::log(p1 / p0) +
        (static_cast<double>(games) - score) *
            std::log((1.0 - p1) / (1.0 - p0));
    const double lower = std::log(beta / (1.0 - alpha));
    const double upper = std::log((1.0 - beta) / alpha);
    std::optional<std::string> verdict;
    if (games > 0 && games % 2 == 0) {
        if (llr <= lower) verdict = "accept_h0";
        if (llr >= upper) verdict = "accept_h1";
        if (!verdict && games >= competition.value("games_per_pair", 0U)) {
            verdict = "inconclusive";
        }
    }
    update_resource_data(
        store, kind, id, "competition.runtime.sprt", "competition.sprt_updated",
        [&](json& data) {
            json sprt = data.at("sprt");
            if (sprt.contains("verdict") && !sprt.at("verdict").is_null()) {
                return;
            }
            sprt["llr"] = llr;
            sprt["lower_threshold"] = lower;
            sprt["upper_threshold"] = upper;
            sprt["games"] = games;
            sprt["score"] = score;
            if (verdict) {
                sprt["verdict"] = *verdict;
                data["planned_total_games"] = games;
            }
            data["sprt"] = std::move(sprt);
        });
    return verdict;
}

struct SwissStanding {
    double score{};
    int color_balance{};
    std::set<std::string, std::less<>> opponents;
    bool had_bye{};
};

std::map<std::string, SwissStanding, std::less<>> calculate_swiss_standings(
    const json& competition,
    const std::vector<json>& games) {
    std::map<std::string, SwissStanding, std::less<>> standings;
    for (const auto& engine : competition.at("engine_ids")) {
        standings.emplace(engine.get<std::string>(), SwissStanding{});
    }
    for (const auto& game : games) {
        if (game.value("lifecycle", "") != "completed") continue;
        const std::string white = game.at("white_engine").value("id", "");
        const std::string black = game.at("black_engine").value("id", "");
        if (!standings.contains(white) || !standings.contains(black)) continue;
        standings.at(white).color_balance += 1;
        standings.at(black).color_balance -= 1;
        standings.at(white).opponents.insert(black);
        standings.at(black).opponents.insert(white);
        const std::string result = game.value("result", "*");
        if (result == "1-0") standings.at(white).score += 1.0;
        if (result == "0-1") standings.at(black).score += 1.0;
        if (result == "1/2-1/2") {
            standings.at(white).score += 0.5;
            standings.at(black).score += 0.5;
        }
    }
    for (const auto& bye : competition.value("swiss_byes", json::array())) {
        const std::string engine = bye.value("engine_id", "");
        if (standings.contains(engine)) {
            standings.at(engine).score += bye.value("score", 1.0);
            standings.at(engine).had_bye = true;
        }
    }
    return standings;
}

json swiss_standings_json(
    const std::vector<std::string>& engines,
    const std::map<std::string, SwissStanding, std::less<>>& standings) {
    json projected = json::array();
    for (const auto& engine : engines) {
        projected.push_back({
            {"engine_id", engine},
            {"score", standings.at(engine).score},
            {"color_balance", standings.at(engine).color_balance},
        });
    }
    return projected;
}

void append_swiss_round(
    const std::shared_ptr<ports::Store>& store,
    const std::string& kind,
    const std::string& id,
    const std::uint32_t round) {
    const json competition = competition_data(store, kind, id);
    for (const auto& pairing : competition.at("pairings")) {
        if (pairing.value("round", 0U) == round) return;
    }
    const auto engines =
        competition.at("engine_ids").get<std::vector<std::string>>();
    auto standings = calculate_swiss_standings(
        competition, competition_games(store, id));
    std::map<std::string, std::size_t, std::less<>> seeded_rank;
    for (std::size_t index = 0; index < engines.size(); ++index) {
        seeded_rank[engines[(index + competition.value("seed", 0ULL)) %
                            engines.size()]] = index;
    }
    std::vector<std::string> pool = engines;
    std::sort(
        pool.begin(), pool.end(), [&](const auto& left, const auto& right) {
            if (standings.at(left).score != standings.at(right).score) {
                return standings.at(left).score > standings.at(right).score;
            }
            return seeded_rank.at(left) < seeded_rank.at(right);
        });
    std::optional<std::string> bye_engine;
    if (pool.size() % 2 == 1) {
        auto selected = std::find_if(
            pool.rbegin(), pool.rend(),
            [&](const auto& engine) { return !standings.at(engine).had_bye; });
        if (selected == pool.rend()) selected = pool.rbegin();
        bye_engine = *selected;
        pool.erase(std::find(pool.begin(), pool.end(), *bye_engine));
    }
    std::uint32_t sequence = 1;
    for (const auto& pairing : competition.at("pairings")) {
        sequence = std::max(sequence, pairing.value("sequence", 0U) + 1U);
    }
    json added = json::array();
    std::uint32_t board = 1;
    while (!pool.empty()) {
        const std::string first = pool.front();
        pool.erase(pool.begin());
        auto selected = pool.begin();
        for (auto candidate = pool.begin(); candidate != pool.end(); ++candidate) {
            const auto cost = std::tuple{
                standings.at(first).opponents.contains(*candidate) ? 1 : 0,
                std::abs(standings.at(first).score - standings.at(*candidate).score),
                seeded_rank.at(*candidate)};
            const auto selected_cost = std::tuple{
                standings.at(first).opponents.contains(*selected) ? 1 : 0,
                std::abs(standings.at(first).score - standings.at(*selected).score),
                seeded_rank.at(*selected)};
            if (cost < selected_cost) selected = candidate;
        }
        const std::string second = *selected;
        pool.erase(selected);
        const int first_white_cost =
            std::abs(standings.at(first).color_balance + 1) +
            std::abs(standings.at(second).color_balance - 1);
        const int second_white_cost =
            std::abs(standings.at(first).color_balance - 1) +
            std::abs(standings.at(second).color_balance + 1);
        const bool first_white = first_white_cost < second_white_cost ||
            (first_white_cost == second_white_cost &&
             ((seeded_rank.at(first) + round) % 2 == 0));
        added.push_back({
            {"sequence", sequence++},
            {"white_engine_id", first_white ? first : second},
            {"black_engine_id", first_white ? second : first},
            {"round", round},
            {"board", board},
            {"opening_index",
             competition.value("seed", 0ULL) +
                 (static_cast<std::uint64_t>(round - 1U) * engines.size() / 2U) +
                 board - 1U},
        });
        ++board;
    }
    update_resource_data(
        store, kind, id, "competition.runtime.swiss_pair", "competition.swiss_round_paired",
        [&](json& data) {
            for (const auto& pairing : data.at("pairings")) {
                if (pairing.value("round", 0U) == round) return;
            }
            for (const auto& pairing : added) data["pairings"].push_back(pairing);
            json byes = data.value("swiss_byes", json::array());
            if (bye_engine) {
                byes.push_back({
                    {"round", round},
                    {"engine_id", *bye_engine},
                    {"score", 1.0},
                });
            }
            data["swiss_byes"] = std::move(byes);
            if (bye_engine) standings.at(*bye_engine).score += 1.0;
            data["swiss_standings"] = swiss_standings_json(engines, standings);
        });
}

void persist_final_swiss_standings(
    const std::shared_ptr<ports::Store>& store,
    const std::string& kind,
    const std::string& id) {
    const json competition = competition_data(store, kind, id);
    const auto engines =
        competition.at("engine_ids").get<std::vector<std::string>>();
    const auto standings = calculate_swiss_standings(
        competition, competition_games(store, id));
    update_resource_data(
        store, kind, id, "competition.runtime.swiss_standings",
        "competition.swiss_standings_updated",
        [&](json& data) {
            data["swiss_standings"] = swiss_standings_json(engines, standings);
        });
}

void persist_progress(
    const std::shared_ptr<ports::Store>& store,
    const std::string& kind,
    const std::string& id,
    const std::size_t active_games,
    const std::optional<std::string>& lifecycle = std::nullopt,
    const std::string& failure = {}) {
    const json competition = competition_data(store, kind, id);
    const std::size_t total = competition.value(
        "planned_total_games",
        competition.value("pairings", json::array()).size());
    std::size_t completed = 0;
    std::size_t cancelled = 0;
    std::size_t failed = 0;
    std::optional<std::uint32_t> current_round;
    for (const auto& game : competition_games(store, id)) {
        const std::string game_lifecycle = game.value("lifecycle", "");
        if (game_lifecycle == "completed") {
            ++completed;
        } else if (game_lifecycle == "cancelled") {
            ++cancelled;
        } else if (game_lifecycle == "failed") {
            ++failed;
        } else if (game_lifecycle == "running") {
            current_round = game.value("round", 0U);
        }
    }
    const std::string effective_lifecycle = lifecycle.value_or(
        competition.value("lifecycle", "planned"));
    const bool terminal = effective_lifecycle == "completed" ||
        effective_lifecycle == "cancelled" ||
        effective_lifecycle == "failed";
    const std::size_t terminal_games = completed + cancelled + failed;
    const std::size_t effective_active = total > terminal_games
        ? std::min(active_games, total - terminal_games)
        : 0;
    const std::size_t pending = total > terminal_games + effective_active
        ? total - terminal_games - effective_active
        : 0;
    const std::size_t queued = terminal ? 0 : pending;
    const std::size_t unplayed = terminal ? pending : 0;
    const std::size_t dispositioned = terminal_games + unplayed;
    update_resource_data(
        store,
        kind,
        id,
        "competition.runtime.progress",
        "competition.progressed",
        [&](json& data) {
            if (lifecycle) {
                data["lifecycle"] = *lifecycle;
            }
            data["progress"] = {
                {"total_games", total},
                {"queued_games", queued},
                {"active_games", effective_active},
                {"completed_games", completed},
                {"cancelled_games", cancelled},
                {"failed_games", failed},
                {"unplayed_games", unplayed},
                {"percentage", total == 0 ? 0.0 :
                     (100.0 * static_cast<double>(dispositioned) /
                      static_cast<double>(total))},
                {"current_round",
                 terminal
                     ? json(nullptr)
                     : current_round ? json(*current_round) : json(nullptr)},
                {"updated_at", domain::utc_now()},
            };
            if (!failure.empty()) {
                data["failure_detail"] = failure;
            } else if (lifecycle && *lifecycle != "failed") {
                data.erase("failure_detail");
            }
        });
}

bool terminal_progress_requires_reconciliation(const json& competition) {
    const std::string lifecycle = competition.value("lifecycle", "planned");
    if (lifecycle != "completed" && lifecycle != "cancelled" &&
        lifecycle != "failed") {
        return false;
    }
    if (!competition.contains("progress") ||
        !competition.at("progress").is_object()) {
        return true;
    }
    const json& progress = competition.at("progress");
    for (const auto* field : {
             "total_games", "queued_games", "active_games",
             "completed_games", "cancelled_games", "failed_games",
             "unplayed_games", "percentage"}) {
        if (!progress.contains(field) || !progress.at(field).is_number()) {
            return true;
        }
    }
    const std::size_t total = progress.at("total_games").get<std::size_t>();
    const std::size_t dispositioned =
        progress.at("completed_games").get<std::size_t>() +
        progress.at("cancelled_games").get<std::size_t>() +
        progress.at("failed_games").get<std::size_t>() +
        progress.at("unplayed_games").get<std::size_t>();
    return progress.at("active_games").get<std::size_t>() != 0 ||
        progress.at("queued_games").get<std::size_t>() != 0 ||
        dispositioned != total ||
        (total != 0 && progress.at("percentage").get<double>() != 100.0);
}

}  // namespace

struct TournamentExecutor::RunState {
    std::atomic_bool paused{false};
    std::atomic_bool cancel_requested{false};
    std::atomic_bool shutdown_requested{false};
    std::atomic_bool finished{false};
    std::jthread controller;
};

TournamentExecutor::TournamentExecutor(
    std::shared_ptr<ports::Store> store,
    ComponentInvocationGate component_invocation_gate)
    : store_(std::move(store)),
      component_invocation_gate_(std::move(component_invocation_gate)) {}

TournamentExecutor::~TournamentExecutor() {
    std::vector<std::unique_ptr<RunState>> runs;
    {
        std::scoped_lock lock(mutex_);
        for (auto& [key, state] : runs_) {
            (void)key;
            state->shutdown_requested.store(true);
            state->controller.request_stop();
            runs.push_back(std::move(state));
        }
        runs_.clear();
    }
}

void TournamentExecutor::recover_interrupted() {
    for (const auto& kind : {"match_plan", "tournament"}) {
        for (const auto& record : store_->list_resources(kind, 10'000, 0)) {
            const json data = json::parse(record.data_json);
            const std::string lifecycle = data.value("lifecycle", "planned");
            if (terminal_progress_requires_reconciliation(data)) {
                persist_progress(store_, kind, record.id, 0, lifecycle);
                continue;
            }
            if (lifecycle != "running" && lifecycle != "queued") {
                continue;
            }
            update_resource_data(
                store_,
                kind,
                record.id,
                "competition.runtime.recover",
                "competition.interrupted",
                [](json& current) {
                    current["lifecycle"] = "paused";
                    current["interruption"] =
                        "Manager restarted; resume continues from committed game state.";
                    json progress = current.value("progress", json::object());
                    const std::size_t interrupted =
                        progress.value("active_games", 0U);
                    const std::size_t queued =
                        progress.value("queued_games", 0U);
                    const std::size_t total =
                        progress.value("total_games", 0U);
                    const std::size_t resolved =
                        progress.value("completed_games", 0U) +
                        progress.value("cancelled_games", 0U) +
                        progress.value("failed_games", 0U);
                    const std::size_t remaining = total > resolved
                        ? total - resolved
                        : 0;
                    progress["active_games"] = 0;
                    progress["queued_games"] =
                        std::min(remaining, queued + interrupted);
                    progress["updated_at"] = domain::utc_now();
                    current["progress"] = std::move(progress);
                });
        }
    }
    for (const auto& record : store_->list_resources("game", 10'000, 0)) {
        const json data = json::parse(record.data_json);
        if (data.value("lifecycle", "") != "running") {
            continue;
        }
        update_resource_data(
            store_,
            "game",
            record.id,
            "game.runtime.recover",
            "game.interrupted",
            [](json& current) {
                current["lifecycle"] = "interrupted";
                current["termination"] = "manager_restart";
            });
    }
}

void TournamentExecutor::start(const std::string& kind, const std::string& id) {
    std::unique_ptr<RunState> retired;
    RunState* state = nullptr;
    {
        std::scoped_lock lock(mutex_);
        const std::string key = run_key(kind, id);
        if (const auto existing = runs_.find(key); existing != runs_.end()) {
            if (!existing->second->finished.load()) {
                existing->second->paused.store(false);
                return;
            }
            retired = std::move(existing->second);
            runs_.erase(existing);
        }
        auto created = std::make_unique<RunState>();
        state = created.get();
        runs_.emplace(key, std::move(created));
        state->controller = std::jthread(
            [this, kind, id, state](std::stop_token) {
                run(kind, id, *state);
                state->finished.store(true);
            });
    }
}

void TournamentExecutor::pause(const std::string& kind, const std::string& id) {
    std::scoped_lock lock(mutex_);
    if (const auto existing = runs_.find(run_key(kind, id));
        existing != runs_.end()) {
        existing->second->paused.store(true);
    }
}

void TournamentExecutor::resume(const std::string& kind, const std::string& id) {
    start(kind, id);
}

void TournamentExecutor::cancel(const std::string& kind, const std::string& id) {
    std::scoped_lock lock(mutex_);
    if (const auto existing = runs_.find(run_key(kind, id));
        existing != runs_.end()) {
        existing->second->cancel_requested.store(true);
        existing->second->paused.store(false);
    }
}

void TournamentExecutor::run(
    const std::string& kind,
    const std::string& id,
    RunState& state) {
    try {
        json competition = competition_data(store_, kind, id);
        if (competition.contains("opening_suite") &&
            !competition.at("opening_suite").is_null()) {
            competition["resolved_opening_positions"] = load_opening_suite(
                competition.at("opening_suite").get<std::string>());
        }
        validate_competition_engines(
            store_, competition, component_invocation_gate_);
        const std::size_t concurrency = std::clamp<std::size_t>(
            competition.value("concurrency", 1U), 1, 64);
        std::string failure;
        const auto run_batch = [&](const std::vector<json>& batch,
                                   const std::size_t batch_concurrency) {
            std::vector<json> pending;
            for (const auto& pairing : batch) {
                const auto existing = existing_game(
                    store_, id, pairing.at("sequence").get<std::uint32_t>());
                if (existing &&
                    json::parse(existing->data_json).value("lifecycle", "") ==
                        "completed") {
                    continue;
                }
                pending.push_back(pairing);
            }
            std::vector<std::future<void>> active;
            std::size_t cursor = 0;
            std::size_t last_reported_active = 0;
            while (cursor < pending.size() || !active.empty()) {
                const bool stopping = state.shutdown_requested.load();
                bool progress_changed = false;
                if (!stopping && !state.cancel_requested.load() &&
                    !state.paused.load() && failure.empty()) {
                    while (cursor < pending.size() &&
                           active.size() < batch_concurrency) {
                        const json pairing = pending[cursor++];
                        active.push_back(std::async(
                            std::launch::async,
                            [this, kind, id, competition, pairing, &state]() {
                                const auto white = game_engine(
                                    store_, competition,
                                    pairing.at("white_engine_id").get<std::string>(),
                                    component_invocation_gate_);
                                const auto black = game_engine(
                                    store_, competition,
                                    pairing.at("black_engine_id").get<std::string>(),
                                    component_invocation_gate_);
                                auto game = existing_game(
                                    store_, id,
                                    pairing.at("sequence").get<std::uint32_t>());
                                if (!game) {
                                    game = create_game(
                                        store_, kind, id, competition, pairing,
                                        white, black);
                                } else {
                                    update_resource_data(
                                        store_, "game", game->id,
                                        "game.runtime.resume", "game.resumed",
                                        [](json& data) {
                                            data["lifecycle"] = "running";
                                            data["termination"] = "in_progress";
                                        });
                                    game = store_->get_resource("game", game->id);
                                }
                                const json game_data = json::parse(game->data_json);
                                const json time = competition.at("time_control");
                                const json adjudication = competition.value(
                                    "adjudication", json::object());
                                adapters::UciGameRequest request;
                                request.white = white;
                                request.black = black;
                                request.initial_time_ms =
                                    time.value("initial_ms", 60'000ULL);
                                request.increment_ms =
                                    time.value("increment_ms", 600ULL);
                                if (time.contains("moves_to_go") &&
                                    !time.at("moves_to_go").is_null()) {
                                    request.moves_to_go =
                                        time.at("moves_to_go").get<std::uint32_t>();
                                }
                                request.starting_fen =
                                    game_data.at("starting_fen").get<std::string>();
                                request.chess960 =
                                    game_data.value("variant", "standard") == "chess960";
                                request.max_plies =
                                    adjudication.value("max_plies", 512U);
                                request.draw_consecutive_plies =
                                    adjudication.value("draw_consecutive_plies", 8U);
                                request.draw_min_plies =
                                    adjudication.value("draw_min_plies", 80U);
                                request.win_consecutive_plies =
                                    adjudication.value("win_consecutive_plies", 6U);
                                if (adjudication.contains("draw_cp") &&
                                    !adjudication.at("draw_cp").is_null()) {
                                    request.draw_cp = adjudication.at("draw_cp")
                                                          .get<std::int32_t>();
                                }
                                if (adjudication.contains("win_cp") &&
                                    !adjudication.at("win_cp").is_null()) {
                                    request.win_cp = adjudication.at("win_cp")
                                                         .get<std::int32_t>();
                                }
                                request.resume = persisted_game_state(game_data);
                                const auto result = adapters::UciGameRunner::play(
                                    request,
                                    [&state]() {
                                        return state.cancel_requested.load() ||
                                               state.shutdown_requested.load();
                                    },
                                    [this, game_id = game->id](
                                        const adapters::UciGameState& current) {
                                        persist_ply(store_, game_id, current);
                                    });
                                persist_game_result(
                                    store_, game->id, result,
                                    state.cancel_requested.load());
                                if (result.lifecycle == "failed") {
                                    throw std::runtime_error(
                                        result.detail.empty()
                                            ? "UCI game failed"
                                            : result.detail);
                                }
                            }));
                        progress_changed = true;
                    }
                }

                for (auto iterator = active.begin(); iterator != active.end();) {
                    if (iterator->wait_for(std::chrono::milliseconds(0)) !=
                        std::future_status::ready) {
                        ++iterator;
                        continue;
                    }
                    try {
                        iterator->get();
                    } catch (const std::exception& error) {
                        failure = error.what();
                        state.cancel_requested.store(true);
                    }
                    iterator = active.erase(iterator);
                    progress_changed = true;
                }
                if (progress_changed || active.size() != last_reported_active) {
                    persist_progress(store_, kind, id, active.size());
                    last_reported_active = active.size();
                }
                if ((state.cancel_requested.load() ||
                     state.shutdown_requested.load() || !failure.empty()) &&
                    active.empty()) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        };

        const std::string format = competition.value("format", "head_to_head");
        if (format == "swiss") {
            const std::uint32_t rounds = competition.value("rounds", 1U);
            for (std::uint32_t round = 1; round <= rounds; ++round) {
                if (round > 1) {
                    append_swiss_round(store_, kind, id, round);
                }
                competition = competition_data(store_, kind, id);
                if (competition.contains("opening_suite") &&
                    !competition.at("opening_suite").is_null()) {
                    competition["resolved_opening_positions"] =
                        load_opening_suite(
                            competition.at("opening_suite").get<std::string>());
                }
                std::vector<json> round_pairings;
                for (const auto& pairing : competition.at("pairings")) {
                    if (pairing.value("round", 0U) == round) {
                        round_pairings.push_back(pairing);
                    }
                }
                run_batch(round_pairings, concurrency);
                if (!failure.empty() || state.cancel_requested.load() ||
                    state.shutdown_requested.load()) {
                    break;
                }
            }
            if (failure.empty() && !state.cancel_requested.load() &&
                !state.shutdown_requested.load()) {
                persist_final_swiss_standings(store_, kind, id);
            }
        } else if (format == "sprt") {
            const auto pairings =
                competition.at("pairings").get<std::vector<json>>();
            for (std::size_t cursor = 0; cursor < pairings.size(); cursor += 2) {
                const std::size_t end = std::min(cursor + 2, pairings.size());
                run_batch(
                    std::vector<json>(pairings.begin() +
                                          static_cast<std::ptrdiff_t>(cursor),
                                      pairings.begin() +
                                          static_cast<std::ptrdiff_t>(end)),
                    std::min<std::size_t>(2, concurrency));
                if (!failure.empty() || state.cancel_requested.load() ||
                    state.shutdown_requested.load()) {
                    break;
                }
                if (persist_sprt_state(store_, kind, id)) {
                    break;
                }
            }
        } else {
            run_batch(
                competition.at("pairings").get<std::vector<json>>(),
                concurrency);
        }

        if (state.shutdown_requested.load()) {
            return;
        }
        if (!failure.empty()) {
            persist_progress(store_, kind, id, 0, "failed", failure);
        } else if (state.cancel_requested.load()) {
            persist_progress(store_, kind, id, 0, "cancelled");
        } else {
            persist_progress(store_, kind, id, 0, "completed");
        }
    } catch (const std::exception& error) {
        if (!state.shutdown_requested.load()) {
            try {
                persist_progress(store_, kind, id, 0, "failed", error.what());
            } catch (...) {
            }
        }
    }
}

}  // namespace arena::application
