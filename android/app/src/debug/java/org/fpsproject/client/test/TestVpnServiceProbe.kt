package org.fpsproject.client.test

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

object TestVpnServiceProbe {
    private val lock = Any()
    private var permissionLatch = CountDownLatch(1)
    private var establishLatch = CountDownLatch(1)
    private var closedLatch = CountDownLatch(1)
    private var permissionResult: TestVpnPermissionResult? = null
    private var establishResult: TestVpnEstablishResult? = null
    private var closed = false

    fun reset() {
        synchronized(lock) {
            permissionLatch = CountDownLatch(1)
            establishLatch = CountDownLatch(1)
            closedLatch = CountDownLatch(1)
            permissionResult = null
            establishResult = null
            closed = false
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
}
