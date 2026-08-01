#pragma once

#include "arena/ports/store.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace arena::application {

class TournamentExecutor final {
  public:
    explicit TournamentExecutor(std::shared_ptr<ports::Store> store);
    ~TournamentExecutor();

    TournamentExecutor(const TournamentExecutor&) = delete;
    TournamentExecutor& operator=(const TournamentExecutor&) = delete;

    void recover_interrupted();
    void start(const std::string& kind, const std::string& id);
    void pause(const std::string& kind, const std::string& id);
    void resume(const std::string& kind, const std::string& id);
    void cancel(const std::string& kind, const std::string& id);

  private:
    struct RunState;

    void run(const std::string& kind, const std::string& id, RunState& state);

    std::shared_ptr<ports::Store> store_;
    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<RunState>, std::less<>> runs_;
};

}  // namespace arena::application
