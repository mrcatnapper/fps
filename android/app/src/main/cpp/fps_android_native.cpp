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
