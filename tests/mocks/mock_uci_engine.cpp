#include <iostream>
#include <string>

int main() {
    for (std::string line; std::getline(std::cin, line);) {
        if (line == "uci") {
            std::cout << "id name Arena Mock Engine\n"
                      << "id author UCI Arena Manager\n"
                      << "option name Threads type spin default 1 min 1 max 16\n"
                      << "option name Style type combo default Normal var Solid var Normal var Risky\n"
                      << "uciok\n"
                      << std::flush;
        } else if (line == "isready") {
            std::cout << "readyok\n" << std::flush;
        } else if (line.starts_with("go ")) {
            std::cout << "info depth 1 score cp 12 nodes 20 pv e2e4\n"
                      << "bestmove e2e4\n"
                      << std::flush;
        } else if (line == "quit") {
            return 0;
        }
    }
    return 0;
}
