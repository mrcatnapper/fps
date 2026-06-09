plugins {
    id("com.android.application")
}

android {
    namespace = "org.fpsproject.client"
    compileSdk = 36

    defaultConfig {
        applicationId = "org.fpsproject.client"
        minSdk = 29
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf("-DCMAKE_BUILD_TYPE=Release")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    testOptions {
        managedDevices {
            localDevices {
                create("fpsApi30Atd") {
                    device = "Pixel 2"
                    apiLevel = 30
                    systemImageSource = "aosp-atd"
                    require64Bit = true
                    testedAbi = "x86_64"
                }
            }
        }
    }
}

// AGP 9.0.1 exposes ManagedVirtualDevice.testedAbi in the public DSL, but its
// setup task creation action does not copy that value into the task input. Keep
// this narrow workaround until AGP propagates the property itself.
tasks.matching { it.name == "fpsApi30AtdSetup" }.configureEach {
    @Suppress("UNCHECKED_CAST")
    val testedAbiProperty = javaClass.methods
        .singleOrNull { it.name == "getTestedAbi" && it.parameterCount == 0 }
        ?.invoke(this) as? org.gradle.api.provider.Property<String>
    testedAbiProperty?.set("x86_64")
}

dependencies {
    implementation(platform("com.squareup.okhttp3:okhttp-bom:5.3.2"))
    implementation("com.squareup.okhttp3:okhttp")

    testImplementation(platform("com.squareup.okhttp3:okhttp-bom:5.3.2"))
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.json:json:20240303")
    testImplementation("com.squareup.okhttp3:mockwebserver3")
    testImplementation("com.squareup.okhttp3:okhttp-tls")

    androidTestImplementation(platform("com.squareup.okhttp3:okhttp-bom:5.3.2"))
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("androidx.test.uiautomator:uiautomator:2.3.0")
}
