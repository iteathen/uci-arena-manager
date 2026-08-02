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
        }
    }
    std::string position;
    bool chess960_enabled = false;
    bool model_root_ready = required_model_root.empty();
    const std::vector<std::string> scholar_mate{
        "e2e4", "e7e5", "d1h5", "b8c6", "f1c4", "g8f6", "h5f7"};
    for (std::string line; std::getline(std::cin, line);) {
        if (line == "uci") {
            std::cout << "id name Arena Mock Engine\n"
                      << "id author UCI Arena Manager\n"
                      << "option name Threads type spin default 1 min 1 max 16\n"
                      << "option name Style type combo default Normal var Solid var Normal var Risky\n";
            if (!required_model_root.empty()) {
                std::cout << "option name ModelRoot type string default missing\n";
            } else {
                std::cout << "info string model loaded:backend:onnx:path:fixture:fingerprint:8104116015256418347:provider:cpu:provider_numeric_fingerprint:5805769137532863231\n";
            }
            std::cout << "uciok\n" << std::flush;
        } else if (line.starts_with("setoption name ModelRoot value ")) {
            model_root_ready =
                line.substr(std::string("setoption name ModelRoot value ").size()) ==
                required_model_root;
        } else if (line == "isready") {
            if (!required_model_root.empty() && model_root_ready) {
                std::cout << "info string model loaded:backend:onnx:path:fixture:fingerprint:8104116015256418347:provider:cpu:provider_numeric_fingerprint:5805769137532863231\n";
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
                        !model_root_ready ||
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
