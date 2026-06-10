#include <jni.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "android_jni_helpers.hpp"
#include "android_native_runtime.hpp"
#include "fps/core/crypto.hpp"
#include "fps/core/enum.hpp"
#include "fps/log/logging.hpp"
#include "fps/net/covert_datagram_transport.hpp"
#include "fps/net/tcp_socket_protector.hpp"
#include "fps/net/tls_tcp_carrier_session.hpp"
#include "fps/net/tun_packet.hpp"

namespace {

[[nodiscard]] auto ignored_trace_value() noexcept -> int { return 42; }

[[nodiscard]] auto string_array(JNIEnv* env, const std::vector<std::string>& values) -> jobjectArray {
    auto* string_class = env->FindClass("java/lang/String");
    if(string_class == nullptr) {
        return nullptr;
    }
    auto* array = env->NewObjectArray(static_cast<jsize>(values.size()), string_class, nullptr);
    if(array == nullptr) {
        env->DeleteLocalRef(string_class);
        return nullptr;
    }
    for(std::size_t index = 0; index < values.size(); ++index) {
        auto* value = env->NewStringUTF(values[index].c_str());
        if(value == nullptr) {
            env->DeleteLocalRef(array);
            env->DeleteLocalRef(string_class);
            return nullptr;
        }
        env->SetObjectArrayElement(array, static_cast<jsize>(index), value);
        env->DeleteLocalRef(value);
        if(env->ExceptionCheck() == JNI_TRUE) {
            env->DeleteLocalRef(array);
            env->DeleteLocalRef(string_class);
            return nullptr;
        }
    }
    env->DeleteLocalRef(string_class);
    return array;
}

[[nodiscard]] auto core_smoke() -> std::string {
    auto random = fps::random_bytes(32);
    if(!random) {
        return "random_bytes:" + std::string{fps::enum_name_or(random.error())};
    }

    auto key_pair = fps::random_x25519_key_pair();
    if(!key_pair) {
        return "x25519_keypair:" + std::string{fps::enum_name_or(key_pair.error())};
    }
    auto derived_public = fps::x25519_public_from_private(key_pair.value().private_key);
    if(!derived_public) {
        return "x25519_public:" + std::string{fps::enum_name_or(derived_public.error())};
    }
    if(!std::equal(derived_public.value().begin(), derived_public.value().end(), key_pair.value().public_key.begin(), key_pair.value().public_key.end())) {
        return "x25519_public:mismatch";
    }

    const std::array<std::byte, 8> hkdf_info{
        std::byte{0x66}, std::byte{0x70}, std::byte{0x73}, std::byte{0x2f}, std::byte{0x61}, std::byte{0x6e}, std::byte{0x64}, std::byte{0x72},
    };
    auto hkdf = fps::hkdf_sha256(random.value(), {}, hkdf_info, fps::kAeadKeySize);
    if(!hkdf) {
        return "hkdf:" + std::string{fps::enum_name_or(hkdf.error())};
    }

    fps::AeadKey aead_key{};
    std::copy(hkdf.value().begin(), hkdf.value().begin() + static_cast<std::ptrdiff_t>(aead_key.size()), aead_key.begin());
    const std::array<std::byte, fps::kAeadNonceSize> nonce{
        std::byte{0x00}, std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}, std::byte{0x05},
        std::byte{0x06}, std::byte{0x07}, std::byte{0x08}, std::byte{0x09}, std::byte{0x0a}, std::byte{0x0b},
    };
    const std::array<std::byte, 3> aad{std::byte{0x61}, std::byte{0x61}, std::byte{0x64}};
    const std::array<std::byte, 5> plaintext{
        std::byte{0x66}, std::byte{0x70}, std::byte{0x73}, std::byte{0x2d}, std::byte{0x31},
    };
    auto encrypted = fps::aead_chacha20_poly1305_encrypt(aead_key, nonce, aad, plaintext);
    if(!encrypted) {
        return "aead_encrypt:" + std::string{fps::enum_name_or(encrypted.error())};
    }
    auto decrypted = fps::aead_chacha20_poly1305_decrypt(aead_key, nonce, aad, encrypted.value().ciphertext, encrypted.value().tag);
    if(!decrypted) {
        return "aead_decrypt:" + std::string{fps::enum_name_or(decrypted.error())};
    }
    if(decrypted.value().size() != plaintext.size() || !std::equal(decrypted.value().begin(), decrypted.value().end(), plaintext.begin(), plaintext.end())) {
        return "aead_decrypt:mismatch";
    }

    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket socket{io_context};
    const auto protector = fps::net::make_noop_tcp_socket_protector();
    if(!protector) {
        return "asio:protector";
    }
    const fps::net::CovertDatagramTransport transport{fps::net::CovertDatagramTransportConfig{}};
    if(transport.carrier_count() != 0U) {
        return "covert_datagram_transport:unexpected_carrier";
    }
    const fps::net::TlsTcpCarrierSessionConfig carrier_config{};
    if(carrier_config.read_buffer_size == 0U) {
        return "tls_tcp_carrier_session_config:invalid";
    }
    socket.close();
    return "ok";
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_nativeVersion(JNIEnv* env, jobject /* self */) {
    fps::log::init_console_logging({.level = fps::log::Severity::warning});
    FPS_LOG_TRACE("android") << "event=native_version ignored=" << ignored_trace_value();
    return env->NewStringUTF("fps-android-native/0.1");
}

extern "C" JNIEXPORT jstring JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_nativeCoreSmoke(JNIEnv* env, jobject /* self */) {
    const auto result = core_smoke();
    return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT jobject JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_parseIpv4FlowTuple(JNIEnv* env, jobject /* self */, jbyteArray packet) {
    auto bytes = fps::android_jni::byte_array_to_bytes(env, packet);
    if(!bytes) {
        return nullptr;
    }
    const auto parsed = fps::net::parse_ipv4_flow_tuple(std::span<const std::byte>{bytes->data(), bytes->size()});
    if(!parsed) {
        return nullptr;
    }
    return fps::android_jni::tun_flow_tuple_object(env, parsed.value());
}

extern "C" JNIEXPORT jlong JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_createRuntime(JNIEnv* env, jobject /* self */, jstring profile_text) {
    auto profile = fps::android_jni::jstring_to_string(env, profile_text);
    if(!profile || profile->empty()) {
        return 0;
    }
    return static_cast<jlong>(fps::android_native::create_runtime(std::move(profile.value())));
}

extern "C" JNIEXPORT void JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_closeRuntime(JNIEnv* /* env */, jobject /* self */, jlong handle) {
    fps::android_native::close_runtime(static_cast<fps::android_native::NativeRuntimeHandle>(handle));
}

extern "C" JNIEXPORT jobject JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_startRuntime(JNIEnv* env, jobject /* self */, jlong handle) {
    const auto snapshot = fps::android_native::start_runtime(static_cast<fps::android_native::NativeRuntimeHandle>(handle));
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobject JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_stopRuntime(JNIEnv* env, jobject /* self */, jlong handle) {
    const auto snapshot = fps::android_native::stop_runtime(static_cast<fps::android_native::NativeRuntimeHandle>(handle));
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobject JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_runtimeSnapshot(JNIEnv* env, jobject /* self */, jlong handle) {
    const auto snapshot = fps::android_native::runtime_snapshot(static_cast<fps::android_native::NativeRuntimeHandle>(handle));
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobject JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_startTunPump(JNIEnv* env, jobject /* self */, jlong handle) {
    const auto snapshot = fps::android_native::start_tun_pump(static_cast<fps::android_native::NativeRuntimeHandle>(handle));
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobject JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_stopTunPump(JNIEnv* env, jobject /* self */, jlong handle) {
    const auto snapshot = fps::android_native::stop_tun_pump(static_cast<fps::android_native::NativeRuntimeHandle>(handle));
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobject JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_postNoopCommand(JNIEnv* env, jobject /* self */, jlong handle) {
    const auto snapshot = fps::android_native::post_noop_command(static_cast<fps::android_native::NativeRuntimeHandle>(handle));
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobject JNICALL
Java_org_fpsproject_client_nativebridge_FpsNative_attachTunFdOwnedDuplicate(JNIEnv* env, jobject /* self */, jlong handle, jint fd, jint mtu) {
    const auto snapshot = fps::android_native::attach_tun_fd_owned_duplicate(
        static_cast<fps::android_native::NativeRuntimeHandle>(handle), static_cast<int>(fd), static_cast<int>(mtu)
    );
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_fpsproject_client_nativebridge_FpsNative_nativeDrainTunPolicyPackets(JNIEnv* env, jobject /* self */, jlong handle, jint max_packets) {
    const auto packets =
        fps::android_native::drain_tun_policy_packets(static_cast<fps::android_native::NativeRuntimeHandle>(handle), static_cast<int>(max_packets));
    return fps::android_jni::tun_policy_packet_array(env, packets);
}

extern "C" JNIEXPORT jobject JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_nativeCompleteTunPolicyPacket(
    JNIEnv* env, jobject /* self */, jlong handle, jlong packet_id, jboolean allow
) {
    const auto snapshot = fps::android_native::complete_tun_policy_packet(
        static_cast<fps::android_native::NativeRuntimeHandle>(handle), static_cast<std::uint64_t>(packet_id), allow == JNI_TRUE
    );
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobject JNICALL
Java_org_fpsproject_client_nativebridge_FpsNative_prepareRawCarrierSocket(JNIEnv* env, jobject /* self */, jlong handle, jstring address, jint port) {
    auto parsed_address = fps::android_jni::jstring_to_string(env, address);
    if(!parsed_address || parsed_address->empty()) {
        const auto snapshot = fps::android_native::invalid_runtime_snapshot("invalid_carrier_endpoint");
        return fps::android_jni::runtime_snapshot_object(env, snapshot);
    }
    const auto snapshot = fps::android_native::prepare_raw_carrier_socket(
        static_cast<fps::android_native::NativeRuntimeHandle>(handle), std::move(parsed_address.value()), static_cast<int>(port)
    );
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobject JNICALL
Java_org_fpsproject_client_nativebridge_FpsNative_completeRawCarrierProtection(JNIEnv* env, jobject /* self */, jlong handle, jboolean protect_allowed) {
    const auto snapshot =
        fps::android_native::complete_raw_carrier_protection(static_cast<fps::android_native::NativeRuntimeHandle>(handle), protect_allowed == JNI_TRUE);
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobject JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_startRawCarrierBridge(JNIEnv* env, jobject /* self */, jlong handle) {
    const auto snapshot = fps::android_native::start_raw_carrier_bridge(static_cast<fps::android_native::NativeRuntimeHandle>(handle));
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobject JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_stopRawCarrier(JNIEnv* env, jobject /* self */, jlong handle) {
    const auto snapshot = fps::android_native::stop_raw_carrier(static_cast<fps::android_native::NativeRuntimeHandle>(handle));
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobject JNICALL Java_org_fpsproject_client_nativebridge_FpsNative_configureClientAuth(
    JNIEnv* env, jobject /* self */, jlong handle, jstring profile_id, jstring client_uuid, jstring server_public_key_base64, jlong client_upgrade_delay_ms,
    jlong client_upgrade_delay_sigma_ms, jint max_frame_payload, jint max_frame_padding
) {
    auto parsed_profile_id = fps::android_jni::jstring_to_string(env, profile_id);
    auto parsed_client_uuid = fps::android_jni::jstring_to_string(env, client_uuid);
    auto parsed_server_public_key = fps::android_jni::jstring_to_string(env, server_public_key_base64);
    if(!parsed_profile_id || !parsed_client_uuid || !parsed_server_public_key) {
        const auto snapshot = fps::android_native::invalid_runtime_snapshot("invalid_client_auth_config");
        return fps::android_jni::runtime_snapshot_object(env, snapshot);
    }
    const auto snapshot = fps::android_native::configure_client_auth(
        static_cast<fps::android_native::NativeRuntimeHandle>(handle), std::move(parsed_profile_id.value()), std::move(parsed_client_uuid.value()),
        std::move(parsed_server_public_key.value()), static_cast<std::int64_t>(client_upgrade_delay_ms), static_cast<std::int64_t>(client_upgrade_delay_sigma_ms),
        static_cast<int>(max_frame_payload), static_cast<int>(max_frame_padding)
    );
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobject JNICALL
Java_org_fpsproject_client_nativebridge_FpsNative_runClientAuthSmokeForTest(JNIEnv* env, jobject /* self */, jlong handle, jboolean tamper_server_accept) {
    const auto snapshot =
        fps::android_native::run_client_auth_smoke_for_test(static_cast<fps::android_native::NativeRuntimeHandle>(handle), tamper_server_accept == JNI_TRUE);
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_fpsproject_client_nativebridge_FpsNative_nativeDrainNativeEvents(JNIEnv* env, jobject /* self */, jlong handle, jint max_events) {
    const auto events = fps::android_native::drain_native_events(static_cast<fps::android_native::NativeRuntimeHandle>(handle), static_cast<int>(max_events));
    return fps::android_jni::native_event_array(env, events);
}

extern "C" JNIEXPORT jobject JNICALL Java_org_fpsproject_client_nativebridge_FpsNativeTestHooks_nativeInstallTunPacketCaptureSinkForTest(
    JNIEnv* env, jobject /* self */, jlong handle, jboolean reject_packets
) {
    const auto snapshot = fps::android_native::install_tun_packet_capture_sink_for_test(
        static_cast<fps::android_native::NativeRuntimeHandle>(handle), reject_packets == JNI_TRUE
    );
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_fpsproject_client_nativebridge_FpsNativeTestHooks_nativeCapturedTunPacketDigestsForTest(JNIEnv* env, jobject /* self */, jlong handle) {
    const auto digests = fps::android_native::captured_tun_packet_digests_for_test(static_cast<fps::android_native::NativeRuntimeHandle>(handle));
    return string_array(env, digests);
}

extern "C" JNIEXPORT jobject JNICALL Java_org_fpsproject_client_nativebridge_FpsNativeTestHooks_nativeStartFakeCarrierForTest(
    JNIEnv* env, jobject /* self */, jlong handle, jboolean reject_frames
) {
    const auto snapshot =
        fps::android_native::start_fake_carrier_for_test(static_cast<fps::android_native::NativeRuntimeHandle>(handle), reject_frames == JNI_TRUE);
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobject JNICALL
Java_org_fpsproject_client_nativebridge_FpsNativeTestHooks_nativeStopFakeCarrierForTest(JNIEnv* env, jobject /* self */, jlong handle) {
    const auto snapshot = fps::android_native::stop_fake_carrier_for_test(static_cast<fps::android_native::NativeRuntimeHandle>(handle));
    return fps::android_jni::runtime_snapshot_object(env, snapshot);
}

extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_fpsproject_client_nativebridge_FpsNativeTestHooks_nativeCapturedFakeCarrierFrameDigestsForTest(JNIEnv* env, jobject /* self */, jlong handle) {
    const auto digests = fps::android_native::captured_fake_carrier_frame_digests_for_test(static_cast<fps::android_native::NativeRuntimeHandle>(handle));
    return string_array(env, digests);
}

extern "C" JNIEXPORT jstring JNICALL Java_org_fpsproject_client_nativebridge_FpsNativeTestHooks_nativeRunZeroRttServerPeerForTest(
    JNIEnv* env, jobject /* self */, jint fd, jstring profile_id, jstring client_uuid, jboolean tamper_server_accept
) {
    auto parsed_profile_id = fps::android_jni::jstring_to_string(env, profile_id);
    auto parsed_client_uuid = fps::android_jni::jstring_to_string(env, client_uuid);
    if(!parsed_profile_id || !parsed_client_uuid) {
        return env->NewStringUTF("invalid_args");
    }
    const auto result = fps::android_native::run_zero_rtt_server_peer_for_test(
        static_cast<int>(fd), std::move(parsed_profile_id.value()), std::move(parsed_client_uuid.value()), tamper_server_accept == JNI_TRUE
    );
    return env->NewStringUTF(result.c_str());
}
