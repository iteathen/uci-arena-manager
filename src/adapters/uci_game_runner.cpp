#include "arena/adapters/uci_game_runner.hpp"

#include "arena/ports/native_process.hpp"

#include <chess.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace arena::adapters {
namespace {

using Clock = std::chrono::steady_clock;

std::vector<std::string> split(const std::string& value) {
    std::istringstream input(value);
    std::vector<std::string> result;
    for (std::string token; input >> token;) {
        result.push_back(std::move(token));
    }
    return result;
}

std::string await_token(
    ports::NativeProcess& process,
    const std::string_view token,
    const Clock::time_point deadline,
    const UciGameRunner::CancellationProbe& cancelled) {
    while (Clock::now() < deadline) {
        if (cancelled()) {
            throw std::runtime_error("game cancelled");
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now());
        const auto line = process.read_line(
            std::min(remaining, std::chrono::milliseconds(100)));
        if (!line) {
            if (!process.running()) {
                throw std::runtime_error(
                    "engine exited before " + std::string(token));
            }
            continue;
        }
        if (*line == token || line->starts_with(std::string(token) + " ")) {
            return *line;
        }
    }
    throw std::runtime_error("engine timed out waiting for " + std::string(token));
}

void initialize_engine(
    ports::NativeProcess& process,
    const UciGameEngine& engine,
    const bool chess960,
    const UciGameRunner::CancellationProbe& cancelled) {
    process.start(engine.executable, engine.working_directory, engine.arguments);
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    process.write_line("uci");
    await_token(process, "uciok", deadline, cancelled);
    if (chess960) {
        process.write_line("setoption name UCI_Chess960 value true");
    }
    for (const auto& [name, value] : engine.options) {
        process.write_line("setoption name " + name + " value " + value);
    }
    process.write_line("isready");
    await_token(process, "readyok", deadline, cancelled);
    process.write_line("ucinewgame");
}

void stop_engine(ports::NativeProcess& process) noexcept {
    try {
        if (process.running()) {
            process.write_line("quit");
        }
        process.stop(std::chrono::seconds(2));
    } catch (...) {
    }
}

std::string position_command(
    const std::string& starting_fen,
    const std::vector<std::string>& moves) {
    std::string command = "position fen " + starting_fen;
    if (!moves.empty()) {
        command += " moves";
        for (const auto& move : moves) {
            command += " ";
            command += move;
        }
    }
    return command;
}

std::string go_command(
    const std::uint64_t white_time,
    const std::uint64_t black_time,
    const std::uint64_t increment,
    const std::optional<std::uint32_t> moves_to_go) {
    std::string command = "go wtime " + std::to_string(white_time) +
           " btime " + std::to_string(black_time) +
           " winc " + std::to_string(increment) +
           " binc " + std::to_string(increment);
    if (moves_to_go) {
        command += " movestogo " + std::to_string(*moves_to_go);
    }
    return command;
}

struct SearchReply {
    std::string bestmove_line;
    std::optional<std::int32_t> score_cp;
    std::vector<std::string> principal_variation;
};

std::optional<std::int32_t> integer_token(const std::string& value) {
    std::int32_t parsed{};
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

SearchReply await_bestmove(
    ports::NativeProcess& process,
    const Clock::time_point deadline,
    const UciGameRunner::CancellationProbe& cancelled) {
    SearchReply result;
    while (Clock::now() < deadline) {
        if (cancelled()) {
            throw std::runtime_error("game cancelled");
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - Clock::now());
        const auto line = process.read_line(
            std::min(remaining, std::chrono::milliseconds(100)));
        if (!line) {
            if (!process.running()) {
                throw std::runtime_error("engine exited before bestmove");
            }
            continue;
        }
        if (line->starts_with("bestmove ") || *line == "bestmove") {
            result.bestmove_line = *line;
            return result;
        }
        if (!line->starts_with("info ")) {
            continue;
        }
        const auto tokens = split(*line);
        const auto score = std::find(tokens.begin(), tokens.end(), "score");
        if (score != tokens.end() && score + 2 < tokens.end() &&
            std::find(score, tokens.end(), "lowerbound") == tokens.end() &&
            std::find(score, tokens.end(), "upperbound") == tokens.end()) {
            if (*(score + 1) == "cp") {
                result.score_cp = integer_token(*(score + 2));
            } else if (*(score + 1) == "mate") {
                if (const auto mate = integer_token(*(score + 2)); mate) {
                    result.score_cp = *mate >= 0 ? 100000 : -100000;
                }
            }
        }
        const auto pv = std::find(tokens.begin(), tokens.end(), "pv");
        if (pv != tokens.end()) {
            result.principal_variation.assign(pv + 1, tokens.end());
        }
    }
    throw std::runtime_error("engine timed out waiting for bestmove");
}

bool draw_adjudicated(
    const UciGameRequest& request,
    const UciGameState& state) {
    if (!request.draw_cp || state.uci_moves.size() < request.draw_min_plies ||
        state.evaluations_cp.size() < request.draw_consecutive_plies) {
        return false;
    }
    const auto begin = state.evaluations_cp.end() -
        static_cast<std::ptrdiff_t>(request.draw_consecutive_plies);
    return std::all_of(
        begin,
        state.evaluations_cp.end(),
        [&](const auto& score) {
            return score &&
                std::abs(static_cast<std::int64_t>(*score)) <=
                    *request.draw_cp;
        });
}

std::optional<std::string> win_adjudicated(
    const UciGameRequest& request,
    const UciGameState& state) {
    if (!request.win_cp ||
        state.evaluations_cp.size() < request.win_consecutive_plies) {
        return std::nullopt;
    }
    const auto begin = state.evaluations_cp.end() -
        static_cast<std::ptrdiff_t>(request.win_consecutive_plies);
    bool white = true;
    bool black = true;
    for (auto current = begin; current != state.evaluations_cp.end(); ++current) {
        white = white && *current && **current >= *request.win_cp;
        black = black && *current && **current <= -*request.win_cp;
    }
    if (white) return "1-0";
    if (black) return "0-1";
    return std::nullopt;
}

std::pair<std::string, std::string> finished_result(
    const chess::Board& board,
    const chess::GameResultReason reason,
    const chess::GameResult result) {
    if (result == chess::GameResult::DRAW) {
        switch (reason) {
            case chess::GameResultReason::STALEMATE:
                return {"1/2-1/2", "stalemate"};
            case chess::GameResultReason::FIFTY_MOVE_RULE:
                return {"1/2-1/2", "fifty_move_rule"};
            case chess::GameResultReason::THREEFOLD_REPETITION:
                return {"1/2-1/2", "threefold_repetition"};
            case chess::GameResultReason::INSUFFICIENT_MATERIAL:
                return {"1/2-1/2", "insufficient_material"};
            default:
                return {"1/2-1/2", "draw"};
        }
    }
    if (result == chess::GameResult::LOSE) {
        return {
            board.sideToMove() == chess::Color::WHITE ? "0-1" : "1-0",
            reason == chess::GameResultReason::CHECKMATE
                ? "checkmate"
                : "game_over"};
    }
    return {"*", "unknown"};
}

}  // namespace

UciGameResult UciGameRunner::play(
    const UciGameRequest& request,
    const CancellationProbe& cancelled,
    const PlyCommit& commit_ply) {
    UciGameResult result;
    result.state = request.resume;
    if (result.state.white_time_ms == 0) {
        result.state.white_time_ms = request.initial_time_ms;
    }
    if (result.state.black_time_ms == 0) {
        result.state.black_time_ms = request.initial_time_ms;
    }

    chess::Board board;
    try {
        if (!(request.chess960
                  ? board.setXfen(request.starting_fen)
                  : board.setFen(request.starting_fen))) {
            throw std::runtime_error("persisted game starting FEN is invalid");
        }
        for (const auto& move_text : result.state.uci_moves) {
            const chess::Move move = chess::uci::uciToMove(board, move_text);
            if (move == chess::Move::NO_MOVE || !board.isLegal(move)) {
                throw std::runtime_error(
                    "persisted game contains an illegal UCI move");
            }
            board.makeMove(move);
        }
        if (result.state.san_moves.size() != result.state.uci_moves.size() ||
            result.state.per_ply_fen.size() != result.state.uci_moves.size() ||
            result.state.evaluations_cp.size() != result.state.uci_moves.size() ||
            result.state.principal_variations.size() !=
                result.state.uci_moves.size()) {
            throw std::runtime_error("persisted game move projections are incomplete");
        }
    } catch (const std::exception& error) {
        result.detail = error.what();
        return result;
    }

    ports::NativeProcess white;
    ports::NativeProcess black;
    try {
        initialize_engine(white, request.white, request.chess960, cancelled);
        initialize_engine(black, request.black, request.chess960, cancelled);

        for (std::size_t ply = result.state.uci_moves.size();
             ply < request.max_plies;
             ++ply) {
            if (cancelled()) {
                result.lifecycle = "cancelled";
                result.termination = "operator_cancelled";
                stop_engine(white);
                stop_engine(black);
                return result;
            }

            const auto [reason, game_result] = board.isGameOver();
            if (reason != chess::GameResultReason::NONE) {
                const auto [score, termination] =
                    finished_result(board, reason, game_result);
                result.lifecycle = "completed";
                result.result = score;
                result.termination = termination;
                stop_engine(white);
                stop_engine(black);
                return result;
            }

            const bool white_to_move =
                board.sideToMove() == chess::Color::WHITE;
            auto& process = white_to_move ? white : black;
            auto& remaining = white_to_move
                ? result.state.white_time_ms
                : result.state.black_time_ms;
            process.write_line(position_command(
                request.starting_fen, result.state.uci_moves));
            const auto started = Clock::now();
            process.write_line(go_command(
                result.state.white_time_ms,
                result.state.black_time_ms,
                request.increment_ms,
                request.moves_to_go));
            SearchReply reply = await_bestmove(
                process,
                started + std::chrono::milliseconds(remaining) +
                    std::chrono::seconds(2),
                cancelled);
            const auto elapsed = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    Clock::now() - started)
                    .count());
            if (elapsed >= remaining) {
                result.lifecycle = "completed";
                result.result = white_to_move ? "0-1" : "1-0";
                result.termination = "time_forfeit";
                stop_engine(white);
                stop_engine(black);
                return result;
            }
            remaining = remaining - elapsed + request.increment_ms;

            const auto parts = split(reply.bestmove_line);
            if (parts.size() < 2 || parts[1] == "0000") {
                throw std::runtime_error(
                    "engine returned no move in a non-terminal position");
            }
            const chess::Move move = chess::uci::uciToMove(board, parts[1]);
            if (move == chess::Move::NO_MOVE || !board.isLegal(move)) {
                result.lifecycle = "completed";
                result.result = white_to_move ? "0-1" : "1-0";
                result.termination = "illegal_move";
                result.detail = "engine returned illegal move " + parts[1];
                stop_engine(white);
                stop_engine(black);
                return result;
            }

            const std::string san = chess::uci::moveToSan(board, move);
            if (reply.score_cp && !white_to_move) {
                *reply.score_cp = -*reply.score_cp;
            }
            board.makeMove(move);
            result.state.uci_moves.push_back(parts[1]);
            result.state.san_moves.push_back(san);
            result.state.per_ply_fen.push_back(board.getFen());
            result.state.evaluations_cp.push_back(reply.score_cp);
            result.state.principal_variations.push_back(
                std::move(reply.principal_variation));
            commit_ply(result.state);
            if (const auto score = win_adjudicated(request, result.state); score) {
                result.lifecycle = "completed";
                result.result = *score;
                result.termination = "evaluation_win_adjudication";
                stop_engine(white);
                stop_engine(black);
                return result;
            }
            if (draw_adjudicated(request, result.state)) {
                result.lifecycle = "completed";
                result.result = "1/2-1/2";
                result.termination = "evaluation_draw_adjudication";
                stop_engine(white);
                stop_engine(black);
                return result;
            }
        }

        result.lifecycle = "completed";
        result.result = "1/2-1/2";
        result.termination = "maximum_ply_adjudication";
    } catch (const std::exception& error) {
        if (std::string_view(error.what()) == "game cancelled") {
            result.lifecycle = "cancelled";
            result.termination = "operator_cancelled";
        } else {
            result.detail = error.what();
        }
    }
    stop_engine(white);
    stop_engine(black);
    return result;
}

}  // namespace arena::adapters
