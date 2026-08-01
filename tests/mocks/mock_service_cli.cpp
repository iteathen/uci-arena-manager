#include <iostream>
#include <string>
#include <string_view>

namespace {

void replace_all(
    std::string& value,
    const std::string_view from,
    const std::string_view to) {
    std::size_t offset = 0;
    while ((offset = value.find(from, offset)) != std::string::npos) {
        value.replace(offset, from.size(), to);
        offset += to.size();
    }
}

bool has_model_root_option(const int argc, char** argv) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) == "--uci-options-json") {
            return std::string_view(argv[index + 1]).find("\"ModelRoot\"") !=
                std::string_view::npos;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        return 2;
    }
    const std::string cli = argv[1];
    const bool evidence = cli.find("evidence") != std::string::npos;
    const std::string command = argv[2];
    const std::string operation = argv[3];
    if (command != "api") {
        return 3;
    }
    if (cli.find("bad-major") != std::string::npos &&
        operation == "describe") {
        std::cout
            << R"({"schema":"uci_arena_manager_service_api_v1","api_version":"2.0","component_id":"book_forge.service","component_version":"0.3.0","operation":"describe","ok":true,"payload":{}})"
            << '\n';
        return 0;
    }
    if (operation == "describe") {
        std::cout
            << (evidence
                    ? R"({"schema":"uci_arena_manager_service_api_v1","api_version":"1.0","component_id":"uci_arena_evidence.service","component_version":"0.3.0","operation":"describe","ok":true,"payload":{"transport":"cli_json","invocation":["api"],"operations":["describe","status","outputs_list","outputs_inspect","config_validate","orchestration_status","target_register","campaign_enqueue","job_control"],"output_schemas":["uci_arena_service_output_v1"],"output_kinds":["evidence.dataset","timing_profile.qualified"],"configuration_schema":"uci_arena_evidence_service_config_v1","status_schema":"uci_arena_evidence_status_v1"}})"
                    : R"({"schema":"uci_arena_manager_service_api_v1","api_version":"1.0","component_id":"book_forge.service","component_version":"0.3.0","operation":"describe","ok":true,"payload":{"transport":"cli_json","invocation":["api"],"operations":["describe","status","outputs_list","outputs_inspect","config_validate"],"output_schemas":["uci_arena_service_output_v1"],"output_kinds":["opening_book.snapshot"],"configuration_schema":"uci_arena_book_forge_config_v1","status_schema":"uci_arena_book_forge_status_v2"}})")
            << '\n';
        return 0;
    }
    if (operation == "status") {
        std::cout
            << (evidence
                    ? R"({"schema":"uci_arena_manager_service_api_v1","api_version":"1.0","component_id":"uci_arena_evidence.service","component_version":"0.3.0","operation":"status","ok":true,"payload":{"status":{"schema":"uci_arena_evidence_status_v1","control":{"paused":false},"collection":{"state":"available","reason":"ready"},"daemon":{"phase":"sleep","stale":false,"lastError":""},"datasets":1}}})"
                    : R"({"schema":"uci_arena_manager_service_api_v1","api_version":"1.0","component_id":"book_forge.service","component_version":"0.3.0","operation":"status","ok":true,"payload":{"status":{"schema":"uci_arena_book_forge_status_v2","control":{"paused":false},"daemon":{"phase":"sleep","stale":false}}}})")
            << '\n';
        return 0;
    }
    if (evidence && operation == "orchestration" && argc > 4 &&
        std::string(argv[4]) == "status") {
        std::cout
            << R"({"schema":"uci_arena_manager_service_api_v1","api_version":"1.0","component_id":"uci_arena_evidence.service","component_version":"0.3.0","operation":"orchestration_status","ok":true,"payload":{"schema":"uci_arena_evidence_orchestration_v1","targets":[{"id":"123e4567-e89b-42d3-a456-426614174001","label":"Active model deployment","model_id":"model-package-1","model_fingerprint":"model-1","engine_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","provider":"directml","created_at":"2026-07-25T00:00:00Z"}],"jobs":[{"id":"123e4567-e89b-42d3-a456-426614174002","adapter":"timing_profile","state":"queued","priority":100,"target_id":"123e4567-e89b-42d3-a456-426614174001","shard_id":"deadline-provider-qualification","dataset_id":"123e4567-e89b-42d3-a456-426614174000","attempts":0,"wait_reason":"","last_error":"","created_at":"2026-07-25T00:00:00Z","updated_at":"2026-07-25T00:00:00Z"}]}})"
            << '\n';
        return 0;
    }
    if (evidence && operation == "campaign" && argc > 4 &&
        std::string(argv[4]) == "enqueue") {
        std::cout
            << R"({"schema":"uci_arena_manager_service_api_v1","api_version":"1.0","component_id":"uci_arena_evidence.service","component_version":"0.3.0","operation":"campaign_enqueue","ok":true,"payload":{"schema":"uci_arena_evidence_campaign_enqueue_v1","campaign":"time_control_curve_v1","target_id":null,"jobs":[]}})"
            << '\n';
        return 0;
    }
    if (evidence && operation == "target" && argc > 4 &&
        std::string(argv[4]) == "register") {
        if (!has_model_root_option(argc, argv)) {
            return 4;
        }
        std::cout
            << R"({"schema":"uci_arena_manager_service_api_v1","api_version":"1.0","component_id":"uci_arena_evidence.service","component_version":"0.3.0","operation":"target_register","ok":true,"payload":{"schema":"uci_arena_evidence_target_register_v1","created":true,"target":{"id":"123e4567-e89b-42d3-a456-426614174003","label":"Example Model deployment","model_id":"example_model_v1","model_fingerprint":"8104116015256418347","engine_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","provider":"cpu","created_at":"2026-07-25T00:00:00Z"}}})"
            << '\n';
        return 0;
    }
    if (evidence && operation == "job" && argc > 4 &&
        std::string(argv[4]) == "control") {
        std::cout
            << R"({"schema":"uci_arena_manager_service_api_v1","api_version":"1.0","component_id":"uci_arena_evidence.service","component_version":"0.3.0","operation":"job_control","ok":true,"payload":{"schema":"uci_arena_evidence_job_control_v1","action":"cancel","job":{"id":"123e4567-e89b-42d3-a456-426614174002","state":"cancelled"}}})"
            << '\n';
        return 0;
    }
    if (operation == "outputs" && argc > 4 &&
        std::string(argv[4]) == "list") {
        std::string response =
            evidence
                ? R"({"schema":"uci_arena_manager_service_api_v1","api_version":"1.0","component_id":"uci_arena_evidence.service","component_version":"0.3.0","operation":"outputs_list","ok":true,"payload":{"cursor":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","unchanged":false,"outputs":[{"schema":"uci_arena_service_output_v1","output_id":"dataset:123e4567-e89b-42d3-a456-426614174000","output_kind":"evidence.dataset","created_at":"2026-07-25T00:00:00Z","state":{"lifecycle":"completed","immutable":true,"completed_at":"2026-07-25T00:01:00Z"},"authority":{"diagnostic":false,"timing":true,"strength":false,"profile_publication":true,"password":"must-not-project"},"compatibility":{"dataset_schema":"uci_arena_evidence_dataset_v1","target_identity":{"model_id":"model-1","fingerprint":"42"},"campaign":"timing"},"summary":{"observation_count":12},"materialization":{"transport":"typed_cli_arguments","arguments":["dataset","export","--id","123e4567-e89b-42d3-a456-426614174000","--out","{manager_destination}","--format","jsonl"]}},{"schema":"uci_arena_service_output_v1","output_id":"profile:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","output_kind":"timing_profile.qualified","created_at":"2026-07-26T00:00:00Z","state":{"immutable":true,"source":"service_live_cache"},"authority":{"diagnostic":false,"timing":true,"strength":false,"profile_publication":true,"provider_safety_qualified":true,"forecast_shadow_qualified":true,"adaptive_play_qualified":false},"compatibility":{"profile_schema":"uci_time_profile_v3","identity_sha256":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","target_identity":{"model_id":"model-1","model_fingerprint":"42","engine_sha256":"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee","provider":"directml"}},"artifact":{"path":"C:/manager/runtime-models/model-1/cache/uci_time_profile_v3.json","sha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"},"provenance":{"service_publication":{"schema":"uci_arena_evidence_profile_publication_v1","dataset_id":"123e4567-e89b-42d3-a456-426614174000","password":"must-not-project"}}}]}})"
                : R"({"schema":"uci_arena_manager_service_api_v1","api_version":"1.0","component_id":"book_forge.service","component_version":"0.3.0","operation":"outputs_list","ok":true,"payload":{"cursor":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","unchanged":false,"outputs":[{"schema":"uci_arena_service_output_v1","output_id":"integration-0123456789abcdefabcd","output_kind":"opening_book.snapshot","created_at":"2026-07-25T00:00:00Z","state":{"channel":"integration","current":true,"immutable":true},"authority":{"installable":true,"strength_qualified":false,"diagnostic_only":false,"includes_fixture":false},"compatibility":{"component_id":"opening_book.standard","consumer_component_id":"uci_arena.engine","snapshot_schema":"uci_arena_book_snapshot_v1"},"summary":{"record_count":2048},"artifact":{"root":"C:/signed/book-snapshot","manifest_path":"C:/signed/book-snapshot/snapshot.manifest.json","manifest_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","files":[{"name":"book.bin","sha256":"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"}]}}]}})";
#ifndef _WIN32
        replace_all(
            response,
            "C:/manager/runtime-models",
            "/manager/runtime-models");
        replace_all(
            response,
            "C:/signed/book-snapshot",
            "/signed/book-snapshot");
#endif
        std::cout << response << '\n';
        return 0;
    }
    return 3;
}
