#pragma once

#include "android_native_runtime.hpp"

#include "fps/net/tun_packet.hpp"

#include <jni.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace fps::android_jni {

[[nodiscard]] auto jstring_to_string(JNIEnv* env, jstring text) -> std::optional<std::string>;
[[nodiscard]] auto byte_array_to_bytes(JNIEnv* env, jbyteArray packet) -> std::optional<std::vector<std::byte>>;
[[nodiscard]] auto tun_flow_tuple_object(JNIEnv* env, const fps::net::TunFlowTuple& tuple) -> jobject;
[[nodiscard]] auto runtime_snapshot_object(JNIEnv* env, const fps::android_native::NativeRuntimeSnapshotFields& snapshot) -> jobject;

} // namespace fps::android_jni
