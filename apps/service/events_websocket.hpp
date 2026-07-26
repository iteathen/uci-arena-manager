#pragma once

#include "arena\application\control_plane.hpp"
#include "arena\application\local_auth.hpp"

#include <drogon/WebSocketController.h>

#include <memory>

namespace arena::service {

class EventsWebSocket final : public drogon::WebSocketController<EventsWebSocket> {
  public:
    static void configure(
        std::shared_ptr<application::ControlPlane> control_plane,
        std::shared_ptr<application::LocalAuth> auth);

    void handleNewMessage(
        const drogon::WebSocketConnectionPtr& connection,
        std::string&& message,
        const drogon::WebSocketMessageType& type) override;
    void handleNewConnection(
        const drogon::HttpRequestPtr& request,
        const drogon::WebSocketConnectionPtr& connection) override;
    void handleConnectionClosed(
        const drogon::WebSocketConnectionPtr& connection) override;

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/api/v1/events");
    WS_PATH_LIST_END

  private:
    static std::shared_ptr<application::ControlPlane> control_plane_;
    static std::shared_ptr<application::LocalAuth> auth_;
};

}  // namespace arena::service
