#pragma once

#include <filesystem>
#include <functional>
#include <memory>

namespace arena::ports {

enum class ComponentChangeKind {
    changed,
    overflow,
};

struct ComponentChange final {
    ComponentChangeKind kind{ComponentChangeKind::changed};
    std::filesystem::path relative_path;
};

class ComponentChangeMonitor {
  public:
    virtual ~ComponentChangeMonitor() = default;
};

using ComponentChangeCallback = std::function<void(const ComponentChange&)>;

[[nodiscard]] std::unique_ptr<ComponentChangeMonitor>
watch_component_tree(const std::filesystem::path& root, ComponentChangeCallback callback);

} // namespace arena::ports
