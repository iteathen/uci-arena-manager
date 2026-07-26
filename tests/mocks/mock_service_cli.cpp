#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        return 2;
    }
    const std::string cli = argv[1];
    const std::string command = argv[2];
    const bool evidence = cli.find("evidence") != std::string::npos;
    if (evidence && command == "status") {
        std::cout
            << R"({"schema":"uci_arena_evidence_status_v1","control":{"paused":false},"collection":{"state":"available","reason":"ready"},"daemon":{"phase":"stopped","lastError":""},"datasets":1})"
            << '\n';
        return 0;
    }
    if (evidence && command == "dataset" && argc > 3 &&
        std::string(argv[3]) == "list") {
        std::cout
            << R"([{"schema":"uci_arena_evidence_dataset_v1","id":"dataset-1","campaign":"timing","state":"completed","targetIdentity":{"model_id":"model-1","fingerprint":"42"},"authority":{"scope":"timing","password":"must-not-project"},"observationCount":12,"createdAt":"2026-07-25T00:00:00Z","completedAt":"2026-07-25T00:01:00Z","observations":[{"raw":"must-not-project"}]}])"
            << '\n';
        return 0;
    }
    if (!evidence && command == "status") {
        std::cout
            << R"({"ok":true,"status":{"schema":"flashfork_book_forge_status_v2","control":{"paused":false},"channels":[{"channel":"integration","snapshot_id":"0123456789abcdefabcd","directory":"C:/signed/book-snapshot","published_at":"2026-07-25T00:00:00Z"}]}})"
            << '\n';
        return 0;
    }
    if (!evidence && command == "validate") {
        std::cout
            << R"({"ok":true,"id":"0123456789abcdefabcd","channel":"integration","manifestSha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","recordCount":2048,"qualified":false,"includesFixture":false})"
            << '\n';
        return 0;
    }
    return 3;
}
