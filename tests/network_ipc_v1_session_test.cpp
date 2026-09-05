#include "ipc/network_ipc_v1_session.h"

#include <cassert>
#include <string>

using namespace network_service::ipc_v1;

namespace {

Frame make_frame(MessageType type, const std::string &payload) {
    Frame frame;
    frame.header.type = type;
    frame.payload = payload;
    return frame;
}

} // namespace

int main() {
    {
        Session session(42, "session-42-1");
        const auto result = session.handle_frame(make_frame(
            MessageType::Hello,
            R"({"minVersion":1,"maxVersion":1,"client":"test","capabilities":[]})"));
        assert(!result.response.empty());
        assert(!result.close_after_send);
        assert(session.ready());
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
        const auto result = session.handle_frame(make_frame(
            MessageType::Hello,
            R"({"minVersion":1,"maxVersion":1)"));
        assert(!result.response.empty());
        assert(result.close_after_send);
    }

    {
        Session session(42, "session-42-5");
        const auto result = session.handle_frame(make_frame(
            MessageType::Hello,
            R"({"minVersion":1,"maxVersion":1,"extra":{"nested":[true,false,null,3.14]}})"));
        assert(!result.response.empty());
        assert(!result.close_after_send);
        assert(session.ready());
    }

    return 0;
}
