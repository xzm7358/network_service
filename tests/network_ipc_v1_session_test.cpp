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

void make_ready(Session &session) {
    const auto result = session.handle_frame(make_frame(
        MessageType::Hello,
        R"({"minVersion":1,"maxVersion":1,"client":"test","capabilities":[]})"));
    assert(!result.response.empty());
    assert(!result.close_after_send);
    assert(session.ready());
}

} // namespace

int main() {
    {
        Session session(42, "session-42-1");
        make_ready(session);
        const Frame response = decode_response(session.handle_frame(make_frame(
            MessageType::Request,
            R"({"requestId":18446744073709551615,"method":"network.ping","params":{}})"))
                                                   .response);
        assert(response.header.type == MessageType::Response);
        assert(response.payload.find("\"requestId\":18446744073709551615") != std::string::npos);
        assert(response.payload.find("\"status\":200") != std::string::npos);
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
