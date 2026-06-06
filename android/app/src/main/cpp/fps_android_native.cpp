#include <jni.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "fps/log/logging.hpp"
#include "fps/net/tun_packet.hpp"

namespace {

[[nodiscard]] auto find_class(JNIEnv* env, const char* name) -> jclass {
    return env->FindClass(name);
}

[[nodiscard]] auto protocol_value(JNIEnv* env, fps::net::TunIpProtocol protocol) -> jobject {
    const auto class_name = "org/fpsproject/client/policy/TunProtocol";
    auto* protocol_class = find_class(env, class_name);
    if(protocol_class == nullptr) {
        return nullptr;
    }
    const auto* field_name = protocol == fps::net::TunIpProtocol::tcp ? "TCP" : "UDP";
    auto* field = env->GetStaticFieldID(protocol_class, field_name, ("L" + std::string{class_name} + ";").c_str());
    if(field == nullptr) {
        return nullptr;
    }
    return env->GetStaticObjectField(protocol_class, field);
}

[[nodiscard]] auto to_tuple_object(JNIEnv* env, const fps::net::TunFlowTuple& tuple) -> jobject {
    auto* tuple_class = find_class(env, "org/fpsproject/client/policy/TunFlowTuple");
    if(tuple_class == nullptr) {
        return nullptr;
    }
    auto* constructor = env->GetMethodID(tuple_class, "<init>", "(Lorg/fpsproject/client/policy/TunProtocol;JIJI)V");
    if(constructor == nullptr) {
        return nullptr;
    }
    auto* protocol = protocol_value(env, tuple.protocol);
    if(protocol == nullptr) {
        return nullptr;
    }
    return env->NewObject(
        tuple_class,
        constructor,
        protocol,
        static_cast<jlong>(tuple.source_ipv4),
        static_cast<jint>(tuple.source_port),
        static_cast<jlong>(tuple.destination_ipv4),
        static_cast<jint>(tuple.destination_port)
    );
}

[[nodiscard]] auto ignored_trace_value() noexcept -> int { return 42; }

} // namespace

extern "C" JNIEXPORT jstring JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_nativeVersion(JNIEnv* env, jobject /* self */) {
    fps::log::init_console_logging({.level = fps::log::Severity::warning});
    FPS_LOG_TRACE("android") << "event=native_version ignored=" << ignored_trace_value();
    return env->NewStringUTF("fps-android-native/0.1");
}

extern "C" JNIEXPORT jobject JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_parseIpv4FlowTuple(
    JNIEnv* env,
    jobject /* self */,
    jbyteArray packet
) {
    if(packet == nullptr) {
        return nullptr;
    }
    const auto size = env->GetArrayLength(packet);
    if(size <= 0) {
        return nullptr;
    }

    auto* raw = env->GetByteArrayElements(packet, nullptr);
    if(raw == nullptr) {
        return nullptr;
    }
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(raw),
        static_cast<std::size_t>(size),
    };
    const auto parsed = fps::net::parse_ipv4_flow_tuple(bytes);
    env->ReleaseByteArrayElements(packet, raw, JNI_ABORT);
    if(!parsed) {
        return nullptr;
    }
    return to_tuple_object(env, parsed.value());
}
