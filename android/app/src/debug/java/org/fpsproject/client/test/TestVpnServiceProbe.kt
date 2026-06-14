package org.fpsproject.client.test

import org.fpsproject.client.nativebridge.NativeVpnRuntimeSnapshot
import org.fpsproject.client.nativebridge.CoordinatedNativeVpnRunnerSnapshot
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

data class TestVpnPermissionResult(
    val granted: Boolean,
)

data class TestVpnEstablishResult(
    val success: Boolean,
    val fd: Int,
    val mtu: Int,
    val error: String?,
)

data class TestNativeRuntimeResult(
    val success: Boolean,
    val error: String?,
    val snapshot: NativeVpnRuntimeSnapshot?,
)

data class TestCoordinatedProductFlowResult(
    val success: Boolean,
    val error: String?,
    val snapshot: CoordinatedNativeVpnRunnerSnapshot?,
)

object TestVpnServiceProbe {
    private val lock = Any()
    private var permissionLatch = CountDownLatch(1)
    private var establishLatch = CountDownLatch(1)
    private var closedLatch = CountDownLatch(1)
    private var nativeRuntimeStartedLatch = CountDownLatch(1)
    private var nativeRuntimeStoppedLatch = CountDownLatch(1)
    private var productFlowStartedLatch = CountDownLatch(1)
    private var productFlowStoppedLatch = CountDownLatch(1)
    private var revokedLatch = CountDownLatch(1)
    private var permissionResult: TestVpnPermissionResult? = null
    private var establishResult: TestVpnEstablishResult? = null
    private var nativeRuntimeStartedResult: TestNativeRuntimeResult? = null
    private var nativeRuntimeStoppedSnapshot: NativeVpnRuntimeSnapshot? = null
    private var productFlowStartedResult: TestCoordinatedProductFlowResult? = null
    private var productFlowStoppedSnapshot: CoordinatedNativeVpnRunnerSnapshot? = null
    private var closed = false
    private var revoked = false

    fun reset() {
        synchronized(lock) {
            permissionLatch = CountDownLatch(1)
            establishLatch = CountDownLatch(1)
            closedLatch = CountDownLatch(1)
            nativeRuntimeStartedLatch = CountDownLatch(1)
            nativeRuntimeStoppedLatch = CountDownLatch(1)
            productFlowStartedLatch = CountDownLatch(1)
            productFlowStoppedLatch = CountDownLatch(1)
            revokedLatch = CountDownLatch(1)
            permissionResult = null
            establishResult = null
            nativeRuntimeStartedResult = null
            nativeRuntimeStoppedSnapshot = null
            productFlowStartedResult = null
            productFlowStoppedSnapshot = null
            closed = false
            revoked = false
        }
    }

    fun reportPermission(granted: Boolean) {
        val latch = synchronized(lock) {
            permissionResult = TestVpnPermissionResult(granted)
            permissionLatch
        }
        latch.countDown()
    }

    fun reportEstablished(fd: Int, mtu: Int) {
        val latch = synchronized(lock) {
            establishResult = TestVpnEstablishResult(success = true, fd = fd, mtu = mtu, error = null)
            establishLatch
        }
        latch.countDown()
    }

    fun reportEstablishFailure(error: String) {
        val latch = synchronized(lock) {
            establishResult = TestVpnEstablishResult(success = false, fd = -1, mtu = 0, error = error)
            establishLatch
        }
        latch.countDown()
    }

    fun reportClosed() {
        val latch = synchronized(lock) {
            closed = true
            closedLatch
        }
        latch.countDown()
    }

    fun reportNativeRuntimeStarted(snapshot: NativeVpnRuntimeSnapshot) {
        val latch = synchronized(lock) {
            nativeRuntimeStartedResult = TestNativeRuntimeResult(success = true, error = null, snapshot = snapshot)
            nativeRuntimeStartedLatch
        }
        latch.countDown()
    }

    fun reportNativeRuntimeFailure(error: String, snapshot: NativeVpnRuntimeSnapshot?) {
        val latch = synchronized(lock) {
            nativeRuntimeStartedResult = TestNativeRuntimeResult(success = false, error = error, snapshot = snapshot)
            nativeRuntimeStartedLatch
        }
        latch.countDown()
    }

    fun reportNativeRuntimeStopped(snapshot: NativeVpnRuntimeSnapshot) {
        val latch = synchronized(lock) {
            nativeRuntimeStoppedSnapshot = snapshot
            nativeRuntimeStoppedLatch
        }
        latch.countDown()
    }

    fun reportProductFlowStarted(snapshot: CoordinatedNativeVpnRunnerSnapshot) {
        val latch = synchronized(lock) {
            productFlowStartedResult = TestCoordinatedProductFlowResult(success = true, error = null, snapshot = snapshot)
            productFlowStartedLatch
        }
        latch.countDown()
    }

    fun reportProductFlowFailure(error: String, snapshot: CoordinatedNativeVpnRunnerSnapshot?) {
        val latch = synchronized(lock) {
            productFlowStartedResult = TestCoordinatedProductFlowResult(success = false, error = error, snapshot = snapshot)
            productFlowStartedLatch
        }
        latch.countDown()
    }

    fun reportProductFlowStopped(snapshot: CoordinatedNativeVpnRunnerSnapshot) {
        val latch = synchronized(lock) {
            productFlowStoppedSnapshot = snapshot
            productFlowStoppedLatch
        }
        latch.countDown()
    }

    fun reportRevoked() {
        val latch = synchronized(lock) {
            revoked = true
            revokedLatch
        }
        latch.countDown()
    }

    fun awaitPermission(timeoutMs: Long): TestVpnPermissionResult? {
        val latch = synchronized(lock) { permissionLatch }
        latch.await(timeoutMs, TimeUnit.MILLISECONDS)
        return synchronized(lock) { permissionResult }
    }

    fun awaitEstablished(timeoutMs: Long): TestVpnEstablishResult? {
        val latch = synchronized(lock) { establishLatch }
        latch.await(timeoutMs, TimeUnit.MILLISECONDS)
        return synchronized(lock) { establishResult }
    }

    fun awaitClosed(timeoutMs: Long): Boolean {
        val latch = synchronized(lock) { closedLatch }
        latch.await(timeoutMs, TimeUnit.MILLISECONDS)
        return synchronized(lock) { closed }
    }

    fun awaitNativeRuntimeStarted(timeoutMs: Long): TestNativeRuntimeResult? {
        val latch = synchronized(lock) { nativeRuntimeStartedLatch }
        latch.await(timeoutMs, TimeUnit.MILLISECONDS)
        return synchronized(lock) { nativeRuntimeStartedResult }
    }

    fun awaitNativeRuntimeStopped(timeoutMs: Long): NativeVpnRuntimeSnapshot? {
        val latch = synchronized(lock) { nativeRuntimeStoppedLatch }
        latch.await(timeoutMs, TimeUnit.MILLISECONDS)
        return synchronized(lock) { nativeRuntimeStoppedSnapshot }
    }

    fun awaitProductFlowStarted(timeoutMs: Long): TestCoordinatedProductFlowResult? {
        val latch = synchronized(lock) { productFlowStartedLatch }
        latch.await(timeoutMs, TimeUnit.MILLISECONDS)
        return synchronized(lock) { productFlowStartedResult }
    }

    fun awaitProductFlowStopped(timeoutMs: Long): CoordinatedNativeVpnRunnerSnapshot? {
        val latch = synchronized(lock) { productFlowStoppedLatch }
        latch.await(timeoutMs, TimeUnit.MILLISECONDS)
        return synchronized(lock) { productFlowStoppedSnapshot }
    }

    fun awaitRevoked(timeoutMs: Long): Boolean {
        val latch = synchronized(lock) { revokedLatch }
        latch.await(timeoutMs, TimeUnit.MILLISECONDS)
        return synchronized(lock) { revoked }
    }
}
