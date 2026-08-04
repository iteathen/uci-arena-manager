#include <iostream>
#include <chrono>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    unsigned delay_ms = 0;
    bool illegal = false;
    std::size_t illegal_after_ply = static_cast<std::size_t>(-1);
    bool require_chess960 = false;
    unsigned required_moves_to_go = 0;
    std::string required_model_root;
    bool duplicate_id_name = false;
    bool malformed_option = false;
    bool require_style_risky = false;
    bool too_many_metadata_lines = false;
    bool oversized_control_line = false;
    bool oversized_info_line = false;
    bool invalid_wire_control = false;
    bool ambiguous_uciok = false;
    bool ambiguous_readyok = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--delay-ms" && index + 1 < argc) {
            delay_ms = static_cast<unsigned>(std::stoul(argv[++index]));
        } else if (argument == "--illegal") {
            illegal = true;
        } else if (argument == "--illegal-after-ply" && index + 1 < argc) {
            illegal_after_ply =
                static_cast<std::size_t>(std::stoull(argv[++index]));
        } else if (argument == "--require-chess960") {
            require_chess960 = true;
        } else if (argument == "--require-movestogo" && index + 1 < argc) {
            required_moves_to_go =
                static_cast<unsigned>(std::stoul(argv[++index]));
        } else if (argument == "--require-model-root" && index + 1 < argc) {
            required_model_root = argv[++index];
        } else if (argument == "--duplicate-id-name") {
            duplicate_id_name = true;
        } else if (argument == "--malformed-option") {
            malformed_option = true;
        } else if (argument == "--require-style-risky") {
            require_style_risky = true;
        } else if (argument == "--too-many-metadata-lines") {
            too_many_metadata_lines = true;
        } else if (argument == "--oversized-control-line") {
            oversized_control_line = true;
        } else if (argument == "--oversized-info-line") {
            oversized_info_line = true;
        } else if (argument == "--invalid-wire-control") {
            invalid_wire_control = true;
        } else if (argument == "--ambiguous-uciok") {
            ambiguous_uciok = true;
        } else if (argument == "--ambiguous-readyok") {
            ambiguous_readyok = true;
        }
    }
    std::string position;
    bool chess960_enabled = false;
    bool model_root_ready = required_model_root.empty();
    bool style_ready = !require_style_risky;
    const std::vector<std::string> scholar_mate{
        "e2e4", "e7e5", "d1h5", "b8c6", "f1c4", "g8f6", "h5f7"};
    for (std::string line; std::getline(std::cin, line);) {
        if (line == "framing-control-burst") {
            for (std::size_t index = 0; index < 32; ++index) {
                std::cout << "control-" << index << "\n";
            }
            std::cout << std::flush;
            return 0;
        } else if (line == "framing-info-burst") {
            for (std::size_t index = 0; index < 16; ++index) {
                std::cout << "info string observation-" << index << "\n";
            }
            std::cout << "done\n" << std::flush;
        } else if (line == "framing-oversized-info") {
            std::cout << "info " << std::string(8'192, 'x') << "\n"
                      << "done\n" << std::flush;
        } else if (line == "uci") {
            if (oversized_info_line) {
                std::cout << "info " << std::string(5'000, 'x') << "\n";
            }
            if (invalid_wire_control) {
                std::cout << "info string invalid";
                std::cout.put('\x01');
                std::cout << "wire\n";
            }
            if (oversized_control_line) {
                std::cout << "id name " << std::string(5'000, 'x') << "\n";
            }
            std::cout << "id name Arena Mock Engine\n";
            if (duplicate_id_name) {
                std::cout << "id name Arena Mock Engine\n";
            }
            std::cout << "id author UCI Arena Manager\n"
                      << "option name Threads type spin default 1 min 1 max 16\n"
                      << "option name Style type combo default Normal var Solid var Normal var Risky\n";
            if (malformed_option) {
                std::cout << "option name Broken type spin default 1 min 0 min 0 max 2\n";
            }
            if (too_many_metadata_lines) {
                for (std::size_t index = 0; index < 1'025; ++index) {
                    std::cout << "id extra " << index << "\n";
                }
            }
            if (!required_model_root.empty()) {
                std::cout << "option name ModelRoot type string default missing\n";
            } else {
                std::cout << "info string model loaded:backend:onnx:path:fixture:fingerprint:8104116015256418347:provider:cpu:provider_numeric_fingerprint:5805769137532863231\n";
            }
            if (ambiguous_uciok) {
                std::cout << "uciok trailing\n" << std::flush;
                return 0;
            }
            std::cout << "uciok\n" << std::flush;
        } else if (line.starts_with("setoption name ModelRoot value ")) {
            model_root_ready =
                line.substr(std::string("setoption name ModelRoot value ").size()) ==
                required_model_root;
        } else if (line == "setoption name Style value Risky") {
            style_ready = true;
        } else if (line == "isready") {
            if (!required_model_root.empty() && model_root_ready) {
                std::cout << "info string model loaded:backend:onnx:path:fixture:fingerprint:8104116015256418347:provider:cpu:provider_numeric_fingerprint:5805769137532863231\n";
            }
            if (ambiguous_readyok) {
                std::cout << "readyok trailing\n" << std::flush;
                return 0;
            }
            std::cout << "readyok\n" << std::flush;
        } else if (line == "setoption name UCI_Chess960 value true") {
            chess960_enabled = true;
        } else if (line.starts_with("position ")) {
            position = line;
        } else if (line.starts_with("go ")) {
            if (delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
            std::size_t move_count = 0;
            const auto marker = position.find(" moves ");
            if (marker != std::string::npos) {
                std::istringstream moves(position.substr(marker + 7));
                for (std::string move; moves >> move;) {
                    ++move_count;
                }
            }
            const bool moves_to_go_present =
                required_moves_to_go == 0 ||
                !position.starts_with("position fen ") ||
                line.find(
                    " movestogo " + std::to_string(required_moves_to_go)) !=
                    std::string::npos;
            const std::string move =
                illegal || move_count >= illegal_after_ply ||
                        !model_root_ready || !style_ready ||
                        (require_chess960 && position.starts_with("position fen ") &&
                         !chess960_enabled) ||
                        !moves_to_go_present
                    ? "a1a1"
                    : move_count < scholar_mate.size()
                        ? scholar_mate[move_count]
                        : "0000";
            std::cout << "info depth 1 score cp 12 nodes 20 pv " << move << "\n"
                      << "bestmove " << move << "\n"
                      << std::flush;
        } else if (line == "quit") {
            return 0;
        }
    }
    return 0;
}
