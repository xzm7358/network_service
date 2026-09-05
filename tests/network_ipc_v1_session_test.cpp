#include "ipc/network_ipc_v1_session.h"

#include <cassert>
#include <cstdint>
#include <string>

using namespace network_service::ipc_v1;

namespace {

Frame make_frame(MessageType type, const std::string &payload) {
    Frame frame;
    frame.header.type = type;
    frame.payload = payload;
    return frame;
}

Frame decode_response(const std::vector<std::uint8_t> &encoded) {
    FrameDecoder decoder;
    assert(decoder.feed(encoded) != DecodeStatus::Error);
    assert(decoder.has_frame());
    return decoder.take_frame();
}

Frame make_ready(Session &session) {
    const auto result = session.handle_frame(make_frame(
        MessageType::Hello,
        R"({"minVersion":1,"maxVersion":1,"client":"test","capabilities":[]})"));
    assert(!result.response.empty());
    assert(!result.close_after_send);
    assert(session.ready());
    return decode_response(result.response);
}

} // namespace

int main() {
    {
        Session session(42, "session-42-1");
        const Frame ready = make_ready(session);
        assert(ready.payload.find("\"request-response\"") != std::string::npos);
        assert(ready.payload.find("\"events\"") != std::string::npos);
        assert(ready.payload.find("\"snapshot-rebase\"") != std::string::npos);
        const Frame response = decode_response(session.handle_frame(make_frame(
            MessageType::Request,
            R"({"requestId":18446744073709551615,"method":"network.ping","params":{}})"))
                                                   .response);
        assert(response.header.type == MessageType::Response);
        assert(response.payload.find("\"requestId\":18446744073709551615") != std::string::npos);
        assert(response.payload.find("\"status\":200") != std::string::npos);
    }

    {
        Session session(42, "session-42-snapshot");
        make_ready(session);
        const auto snapshot = session.handle_frame(make_frame(
            MessageType::Request,
            R"({"requestId":88,"method":"network.snapshot","params":{}})"));
        assert(snapshot.response.empty());
        assert(!snapshot.close_after_send);
        assert(snapshot.server_action == Session::ServerAction::SendAuthoritativeSnapshot);
        assert(snapshot.action_request_id == 88);
        assert(session.ready());
    }

    {
        Session session(42, "session-42-subscribe");
        make_ready(session);
        const auto first = session.handle_frame(make_frame(
            MessageType::Request,
            R"({"requestId":9,"method":"network.events.subscribe","params":{}})"));
        const Frame first_response = decode_response(first.response);
        assert(first_response.header.type == MessageType::Response);
        assert(first_response.payload.find("\"requestId\":9") != std::string::npos);
        assert(first_response.payload.find("\"subscribed\":true") != std::string::npos);
        assert(first.server_action == Session::ServerAction::EmitEventsSubscribed);
        assert(session.events_subscribed());

        const auto duplicate = session.handle_frame(make_frame(
            MessageType::Request,
            R"({"requestId":10,"method":"network.events.subscribe","params":{}})"));
        assert(decode_response(duplicate.response).header.type == MessageType::Response);
        assert(duplicate.server_action == Session::ServerAction::None);
        assert(session.events_subscribed());
    }

    {
        Session session(42, "session-42-2");
        const auto result = session.handle_frame(make_frame(
            MessageType::Request,
            R"({"requestId":1,"method":"network.ping","params":{}})"));
        assert(!result.response.empty());
        assert(result.close_after_send);
        assert(session.state() == Session::State::Closed);
    }

    {
        Session session(42, "session-42-3");
        const auto result = session.handle_frame(make_frame(
            MessageType::Hello,
            R"({"minVersion":2,"maxVersion":3})"));
        assert(!result.response.empty());
        assert(result.close_after_send);
        assert(session.state() == Session::State::Closed);
    }

    {
        Session session(42, "session-42-4");
        make_ready(session);
        const Frame response = decode_response(session.handle_frame(make_frame(
            MessageType::Request,
            R"({"requestId":77,"method":"does.not.exist","params":{}})"))
                                                   .response);
        assert(response.header.type == MessageType::Response);
        assert(response.payload.find("\"requestId\":77") != std::string::npos);
        assert(response.payload.find("\"status\":404") != std::string::npos);
        assert(response.payload.find("\"code\":\"METHOD_NOT_FOUND\"") != std::string::npos);
    }

    {
        Session session(42, "session-42-5");
        make_ready(session);
        const Frame response = decode_response(session.handle_frame(make_frame(
            MessageType::Request,
            R"({"requestId":0,"method":"network.ping","params":{}})"))
                                                   .response);
        assert(response.header.type == MessageType::Error);
        assert(response.payload.find("\"code\":\"INVALID_REQUEST_ID\"") != std::string::npos);
        assert(session.ready());
    }

    {
        Session session(42, "session-42-6");
        make_ready(session);
        const Frame response = decode_response(session.handle_frame(make_frame(
            MessageType::Request,
            R"({"requestId":1.5,"method":"network.ping","params":{}})"))
                                                   .response);
        assert(response.header.type == MessageType::Error);
        assert(response.payload.find("\"code\":\"INVALID_REQUEST_ID\"") != std::string::npos);
    }

    {
        Session session(42, "session-42-7");
        make_ready(session);
        const Frame response = decode_response(session.handle_frame(make_frame(
            MessageType::Request,
            R"({"requestId":18446744073709551616,"method":"network.ping","params":{}})"))
                                                   .response);
        assert(response.header.type == MessageType::Error);
        assert(response.payload.find("\"code\":\"INVALID_REQUEST_ID\"") != std::string::npos);
    }

    return 0;
}
