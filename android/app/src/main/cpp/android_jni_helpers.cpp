#include "android_jni_helpers.hpp"

#include <cstdint>
#include <cstring>
#include <string_view>

namespace fps::android_jni {
namespace {

template <class T> class LocalRef {
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

[[nodiscard]] auto find_local_class(JNIEnv* env, const char* name) -> LocalRef<jclass> {
    return LocalRef<jclass>{env, env->FindClass(name)};
}

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

auto runtime_snapshot_object(JNIEnv* env, const fps::android_native::NativeRuntimeSnapshotFields& snapshot) -> jobject {
    auto snapshot_class = find_local_class(env, "org/fpsproject/client/nativebridge/NativeRuntimeSnapshot");
    if(snapshot_class.get() == nullptr) {
        return nullptr;
    }
    auto* constructor = env->GetMethodID(snapshot_class.get(), "<init>", "(ZZIILjava/lang/String;Ljava/lang/String;)V");
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

    return env->NewObject(
        snapshot_class.get(), constructor, static_cast<jboolean>(snapshot.alive), static_cast<jboolean>(snapshot.tun_attached), static_cast<jint>(snapshot.tun_fd),
        static_cast<jint>(snapshot.tun_mtu), ownership_string.get(), error_string.get()
    );
}

} // namespace fps::android_jni
