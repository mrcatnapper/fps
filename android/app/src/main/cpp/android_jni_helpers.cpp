#include "android_jni_helpers.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>

namespace fps::android_jni {
namespace {

template <class T>
class LocalRef {
public:
    LocalRef(JNIEnv* env, T ref) noexcept : env_{env}, ref_{ref} {}

    LocalRef(const LocalRef&) = delete;
    auto operator=(const LocalRef&) -> LocalRef& = delete;

    LocalRef(LocalRef&& other) noexcept : env_{other.env_}, ref_{other.ref_} { other.ref_ = nullptr; }

    auto operator=(LocalRef&& other) noexcept -> LocalRef& {
        if(this != &other) {
            reset();
            env_ = other.env_;
            ref_ = other.ref_;
            other.ref_ = nullptr;
        }
        return *this;
    }

    ~LocalRef() { reset(); }

    [[nodiscard]] auto get() const noexcept -> T { return ref_; }

private:
    void reset() noexcept {
        if(env_ != nullptr && ref_ != nullptr) {
            env_->DeleteLocalRef(ref_);
        }
        ref_ = nullptr;
    }

    JNIEnv* env_ = nullptr;
    T ref_ = nullptr;
};

[[nodiscard]] auto find_local_class(JNIEnv* env, const char* name) -> LocalRef<jclass> { return LocalRef<jclass>{env, env->FindClass(name)}; }

[[nodiscard]] auto new_optional_string(JNIEnv* env, std::string_view value) -> LocalRef<jstring> {
    if(value.empty()) {
        return LocalRef<jstring>{env, nullptr};
    }
    return LocalRef<jstring>{env, env->NewStringUTF(std::string{value}.c_str())};
}

[[nodiscard]] auto protocol_value(JNIEnv* env, fps::net::TunIpProtocol protocol) -> LocalRef<jobject> {
    constexpr auto class_name = "org/fpsproject/client/policy/TunProtocol";
    auto protocol_class = find_local_class(env, class_name);
    if(protocol_class.get() == nullptr) {
        return LocalRef<jobject>{env, nullptr};
    }
    const auto* field_name = protocol == fps::net::TunIpProtocol::tcp ? "TCP" : "UDP";
    auto* field = env->GetStaticFieldID(protocol_class.get(), field_name, "Lorg/fpsproject/client/policy/TunProtocol;");
    if(field == nullptr) {
        return LocalRef<jobject>{env, nullptr};
    }
    return LocalRef<jobject>{env, env->GetStaticObjectField(protocol_class.get(), field)};
}

} // namespace

auto jstring_to_string(JNIEnv* env, jstring text) -> std::optional<std::string> {
    if(text == nullptr) {
        return std::nullopt;
    }
    const char* raw = env->GetStringUTFChars(text, nullptr);
    if(raw == nullptr) {
        return std::nullopt;
    }
    std::string out{raw};
    env->ReleaseStringUTFChars(text, raw);
    return out;
}

auto byte_array_to_bytes(JNIEnv* env, jbyteArray packet) -> std::optional<std::vector<std::byte>> {
    if(packet == nullptr) {
        return std::nullopt;
    }
    const auto size = env->GetArrayLength(packet);
    if(size <= 0) {
        return std::nullopt;
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    env->GetByteArrayRegion(packet, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
    if(env->ExceptionCheck() == JNI_TRUE) {
        return std::nullopt;
    }
    return bytes;
}

auto tun_flow_tuple_object(JNIEnv* env, const fps::net::TunFlowTuple& tuple) -> jobject {
    auto tuple_class = find_local_class(env, "org/fpsproject/client/policy/TunFlowTuple");
    if(tuple_class.get() == nullptr) {
        return nullptr;
    }
    auto* constructor = env->GetMethodID(tuple_class.get(), "<init>", "(Lorg/fpsproject/client/policy/TunProtocol;JIJI)V");
    if(constructor == nullptr) {
        return nullptr;
    }
    auto protocol = protocol_value(env, tuple.protocol);
    if(protocol.get() == nullptr) {
        return nullptr;
    }
    return env->NewObject(
        tuple_class.get(), constructor, protocol.get(), static_cast<jlong>(tuple.source_ipv4), static_cast<jint>(tuple.source_port),
        static_cast<jlong>(tuple.destination_ipv4), static_cast<jint>(tuple.destination_port)
    );
}

auto tun_policy_packet_array(JNIEnv* env, const std::vector<fps::android_native::NativeTunPolicyPacketFields>& packets) -> jobjectArray {
    auto packet_class = find_local_class(env, "org/fpsproject/client/nativebridge/NativeTunPolicyPacket");
    if(packet_class.get() == nullptr) {
        return nullptr;
    }
    auto* constructor = env->GetMethodID(packet_class.get(), "<init>", "(JILorg/fpsproject/client/policy/TunFlowTuple;)V");
    if(constructor == nullptr) {
        return nullptr;
    }
    auto* array = env->NewObjectArray(static_cast<jsize>(packets.size()), packet_class.get(), nullptr);
    if(array == nullptr) {
        return nullptr;
    }

    for(std::size_t index = 0; index < packets.size(); ++index) {
        auto flow = LocalRef<jobject>{env, tun_flow_tuple_object(env, packets[index].flow)};
        if(flow.get() == nullptr) {
            env->DeleteLocalRef(array);
            return nullptr;
        }
        auto packet = LocalRef<jobject>{
            env,
            env->NewObject(
                packet_class.get(), constructor, static_cast<jlong>(packets[index].packet_id), static_cast<jint>(packets[index].packet_size), flow.get()
            ),
        };
        if(packet.get() == nullptr) {
            env->DeleteLocalRef(array);
            return nullptr;
        }
        env->SetObjectArrayElement(array, static_cast<jsize>(index), packet.get());
        if(env->ExceptionCheck() == JNI_TRUE) {
            env->DeleteLocalRef(array);
            return nullptr;
        }
    }
    return array;
}

auto runtime_snapshot_object(JNIEnv* env, const fps::android_native::NativeRuntimeSnapshotFields& snapshot) -> jobject {
    auto snapshot_class = find_local_class(env, "org/fpsproject/client/nativebridge/NativeRuntimeSnapshot");
    if(snapshot_class.get() == nullptr) {
        return nullptr;
    }
    auto* constructor =
        env->GetMethodID(snapshot_class.get(), "<init>", "(ZZZZZIILjava/lang/String;JJJJLjava/lang/String;JJJJJJJJJJJJJJJJLjava/lang/String;)V");
    if(constructor == nullptr) {
        return nullptr;
    }

    const auto ownership = fps::android_native::tun_fd_ownership_name(snapshot.tun_fd_ownership);
    auto ownership_string = new_optional_string(env, ownership);
    if(!ownership.empty() && ownership_string.get() == nullptr) {
        return nullptr;
    }
    auto error_string = new_optional_string(env, snapshot.last_error);
    if(!snapshot.last_error.empty() && error_string.get() == nullptr) {
        return nullptr;
    }
    auto tun_drop_reason_string = new_optional_string(env, snapshot.tun_last_drop_reason);
    if(!snapshot.tun_last_drop_reason.empty() && tun_drop_reason_string.get() == nullptr) {
        return nullptr;
    }

    return env->NewObject(
        snapshot_class.get(), constructor, static_cast<jboolean>(snapshot.alive), static_cast<jboolean>(snapshot.started),
        static_cast<jboolean>(snapshot.worker_thread_running), static_cast<jboolean>(snapshot.tun_attached), static_cast<jboolean>(snapshot.tun_pump_running),
        static_cast<jint>(snapshot.tun_fd), static_cast<jint>(snapshot.tun_mtu), ownership_string.get(), static_cast<jlong>(snapshot.tun_packets_read),
        static_cast<jlong>(snapshot.tun_bytes_read), static_cast<jlong>(snapshot.tun_packets_parsed), static_cast<jlong>(snapshot.tun_packets_dropped),
        tun_drop_reason_string.get(), static_cast<jlong>(snapshot.tun_policy_pending), static_cast<jlong>(snapshot.tun_policy_in_flight),
        static_cast<jlong>(snapshot.tun_policy_allowed), static_cast<jlong>(snapshot.tun_policy_dropped), static_cast<jlong>(snapshot.tun_policy_queue_full),
        static_cast<jlong>(snapshot.tun_covert_enqueue_attempted), static_cast<jlong>(snapshot.tun_covert_enqueue_accepted),
        static_cast<jlong>(snapshot.tun_covert_enqueue_rejected), static_cast<jlong>(snapshot.commands_posted), static_cast<jlong>(snapshot.commands_completed),
        static_cast<jlong>(snapshot.carrier_active), static_cast<jlong>(snapshot.carrier_started), static_cast<jlong>(snapshot.carrier_stopped),
        static_cast<jlong>(snapshot.carrier_frames_enqueued), static_cast<jlong>(snapshot.carrier_frame_bytes_enqueued),
        static_cast<jlong>(snapshot.carrier_enqueue_rejected), error_string.get()
    );
}

} // namespace fps::android_jni
