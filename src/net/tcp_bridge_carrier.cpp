#include "fps/net/tcp_bridge_carrier.hpp"

#include <vector>

namespace fps::net {
namespace {

[[nodiscard]] auto map_enqueue_error(TcpBridgeEnqueueError error) -> CovertDatagramError {
    switch(error) {
    case TcpBridgeEnqueueError::session_closed:
        return CovertDatagramError::session_closed;
    case TcpBridgeEnqueueError::codec_error:
        return CovertDatagramError::codec_error;
    case TcpBridgeEnqueueError::tls_record_error:
        return CovertDatagramError::tls_record_error;
    case TcpBridgeEnqueueError::write_queue_full:
        return CovertDatagramError::write_queue_full;
    }
    return CovertDatagramError::session_closed;
}

} // namespace

auto make_tcp_bridge_carrier(CarrierId carrier_id, std::weak_ptr<TcpBridgeSession> session) -> CovertCarrier {
    return CovertCarrier{
        .id = carrier_id,
        .enqueue_frames = [session](Direction direction, std::span<const CovertCarrierFrame> frames) -> CovertDatagramResult {
            const auto locked = session.lock();
            if(!locked) {
                return CovertDatagramResult::failure(CovertDatagramError::session_closed);
            }

            std::vector<TcpBridgeCovertFrame> bridge_frames;
            bridge_frames.reserve(frames.size());
            for(const auto& frame : frames) {
                bridge_frames.push_back(
                    TcpBridgeCovertFrame{
                        .frame_type = frame.frame_type,
                        .payload = frame.payload,
                        .padding_size = frame.padding_size,
                        .flags = frame.flags,
                    }
                );
            }

            auto queued = locked->enqueue_covert_frames(direction, bridge_frames);
            if(!queued) {
                return CovertDatagramResult::failure(map_enqueue_error(queued.error()));
            }
            return CovertDatagramResult::success(queued.value());
        },
        .is_alive = [session = std::move(session)] { return !session.expired(); },
    };
}

} // namespace fps::net
