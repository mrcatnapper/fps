package org.fpsproject.client

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

class FpsVpnStatusStoreTest {
    @Test
    fun missingAndCorruptStatusReadAsStopped() {
        val storage = StatusStoreStorage()
        val store = PersistedFpsVpnStatusStore(storage)

        assertEquals(FpsVpnStatusSnapshot.stopped(), store.read())

        storage.value = "{broken"

        assertEquals(FpsVpnStatusSnapshot.stopped(), store.read())
    }

    @Test
    fun statusRoundTripPreservesSafeMetadata() {
        val store = PersistedFpsVpnStatusStore(StatusStoreStorage())

        store.write(
            FpsVpnStatusSnapshot(
                state = FpsVpnStatusState.BACKOFF,
                attempts = 3,
                reconnects = 2,
                nextRetryDelayMs = 2500,
                error = "cover_io_failed",
            ),
        )

        assertEquals(
            FpsVpnStatusSnapshot(
                state = FpsVpnStatusState.BACKOFF,
                attempts = 3,
                reconnects = 2,
                nextRetryDelayMs = 2500,
                error = "cover_io_failed",
            ),
            store.read(),
        )
    }

    @Test
    fun statusReadClampsCountsAndRedactsUnsafeError() {
        val storage = StatusStoreStorage(
            """{"state":"FAILED","attempts":-4,"reconnects":-3,"next_retry_delay_ms":-2,"error":"123e4567-e89b-42d3-a456-426614174000"}""",
        )
        val store = PersistedFpsVpnStatusStore(storage)

        val snapshot = store.read()

        assertEquals(FpsVpnStatusState.FAILED, snapshot.state)
        assertEquals(0, snapshot.attempts)
        assertEquals(0, snapshot.reconnects)
        assertEquals(0, snapshot.nextRetryDelayMs)
        assertEquals("error", snapshot.error)
        assertFalse(snapshot.toString().contains("123e4567-e89b-42d3-a456-426614174000"))
    }

    @Test
    fun persistingNotifierWritesShownSnapshotsAndStoppedOnClear() {
        val storage = StatusStoreStorage()
        val store = PersistedFpsVpnStatusStore(storage)
        val delegate = RecordingStoreStatusNotifier()
        val notifier = PersistingFpsVpnStatusNotifier(delegate, store)

        notifier.show(FpsVpnStatusSnapshot(state = FpsVpnStatusState.RUNNING, attempts = 1))

        assertEquals(FpsVpnStatusState.RUNNING, store.read().state)
        assertEquals(FpsVpnStatusState.RUNNING, delegate.snapshots.single().state)

        notifier.clear()

        assertEquals(FpsVpnStatusSnapshot.stopped(), store.read())
        assertEquals(1, delegate.clearCount)
    }
}

private class StatusStoreStorage(
    var value: String? = null,
) : FpsVpnStatusStorage {
    override fun read(): String? = value

    override fun write(value: String) {
        this.value = value
    }

    override fun clear() {
        value = null
    }
}

private class RecordingStoreStatusNotifier : FpsVpnStatusNotifier {
    val snapshots = mutableListOf<FpsVpnStatusSnapshot>()
    var clearCount = 0

    override fun show(snapshot: FpsVpnStatusSnapshot) {
        snapshots += snapshot
    }

    override fun clear() {
        clearCount += 1
    }
}
