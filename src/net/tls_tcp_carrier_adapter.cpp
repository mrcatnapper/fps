#include "fps/net/tls_tcp_carrier_adapter.hpp"

#include <vector>

namespace fps::net {
namespace {

[[nodiscard]] auto map_enqueue_error(TlsTcpCarrierEnqueueError error) -> CovertDatagramError {
    switch(error) {
    case TlsTcpCarrierEnqueueError::session_closed:
        return CovertDatagramError::session_closed;
    case TlsTcpCarrierEnqueueError::codec_error:
        return CovertDatagramError::codec_error;
    case TlsTcpCarrierEnqueueError::tls_record_error:
        return CovertDatagramError::tls_record_error;
    case TlsTcpCarrierEnqueueError::write_queue_full:
        return CovertDatagramError::write_queue_full;
    }
    return CovertDatagramError::session_closed;
}

} // namespace

auto make_tls_tcp_carrier_adapter(CarrierId carrier_id, std::weak_ptr<TlsTcpCarrierSession> session) -> CovertCarrier {
    return CovertCarrier{
        .id = carrier_id,
        .enqueue_frames = [session](Direction direction, std::span<const CovertCarrierFrame> frames) -> CovertDatagramResult {
            const auto locked = session.lock();
            if(!locked) {
                return CovertDatagramResult::failure(CovertDatagramError::session_closed);
            }

            std::vector<TlsTcpCarrierCovertFrame> carrier_frames;
            carrier_frames.reserve(frames.size());
            for(const auto& frame : frames) {
                carrier_frames.push_back(
                    TlsTcpCarrierCovertFrame{
                        .frame_type = frame.frame_type,
                        .payload = frame.payload,
                        .padding_size = frame.padding_size,
                        .flags = frame.flags,
                    }
                );
            }

            auto queued = locked->enqueue_covert_frames(direction, carrier_frames);
            if(!queued) {
                return CovertDatagramResult::failure(map_enqueue_error(queued.error()));
            }
            return CovertDatagramResult::success(queued.value());
        },
        .is_alive = [session] { return !session.expired(); },
        .can_enqueue_now =
            [session] {
                const auto locked = session.lock();
                return locked && locked->is_enqueue_thread();
            },
    };
}

} // namespace fps::net
