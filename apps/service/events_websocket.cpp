#include "events_websocket.hpp"

#include <nlohmann/json.hpp>

namespace arena::service {
std::shared_ptr<application::ControlPlane> EventsWebSocket::control_plane_;
std::shared_ptr<application::LocalAuth> EventsWebSocket::auth_;

void EventsWebSocket::configure(
    std::shared_ptr<application::ControlPlane> control_plane,
    std::shared_ptr<application::LocalAuth> auth) {
    control_plane_ = std::move(control_plane);
    auth_ = std::move(auth);
}

void EventsWebSocket::handleNewConnection(
    const drogon::HttpRequestPtr& request,
    const drogon::WebSocketConnectionPtr& connection) {
    const auto session = request->getCookie("arena_session");
    if (!auth_ || !auth_->authenticate_session(session)) {
        connection->shutdown(drogon::CloseCode::kViolation, "authentication required");
        return;
    }
    connection->send(
        nlohmann::json{
            {"kind", "connected"},
            {"cursor", 0},
            {"events", control_plane_->events_after(0, 500)},
        }.dump());
}

void EventsWebSocket::handleNewMessage(
    const drogon::WebSocketConnectionPtr& connection,
    std::string&& message,
    const drogon::WebSocketMessageType& type) {
    if (type != drogon::WebSocketMessageType::Text) {
        return;
    }
    try {
        const auto request = nlohmann::json::parse(message);
        const auto cursor = request.value("cursor", 0ULL);
        connection->send(
            nlohmann::json{
                {"kind", "events"},
                {"cursor", cursor},
                {"events", control_plane_->events_after(cursor, 500)},
            }.dump());
    } catch (const std::exception&) {
        connection->send(nlohmann::json{{"kind", "error"}, {"code", "invalid_message"}}.dump());
    }
}

void EventsWebSocket::handleConnectionClosed(
    const drogon::WebSocketConnectionPtr& connection) {
    (void)connection;
}

}  // namespace arena::service
