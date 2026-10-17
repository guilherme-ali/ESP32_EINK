package br.com.ideiarec.companion.protocol;

import static org.junit.Assert.*;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import org.junit.Test;

public class RelayEngineTest {
    private static byte[] open(int channel, int epoch) {
        return openHost(channel, epoch, "api.openai.com", 443);
    }
    private static byte[] openHost(int channel, int epoch, String hostname, int port) {
        byte[] host = hostname.getBytes(StandardCharsets.UTF_8);
        byte[] payload = new byte[host.length + 2]; payload[0] = (byte) port; payload[1] = (byte) (port >>> 8);
        System.arraycopy(host, 0, payload, 2, host.length);
        return new Frame(Frame.OPEN, channel, epoch, 0, payload).encode();
    }
    private static byte[] data(int channel, int epoch, int seq) {
        return new Frame(Frame.DATA, channel, epoch, seq, new byte[]{(byte) seq}).encode();
    }

    private static final class Pipe extends InputStream {
        final ArrayDeque<byte[]> chunks = new ArrayDeque<>();
        boolean closed;
        synchronized void feed(byte[] value) { chunks.add(value.clone()); notifyAll(); }
        @Override public int read() throws IOException { byte[] b = new byte[1]; return read(b) == -1 ? -1 : b[0] & 255; }
        @Override public synchronized int read(byte[] b, int off, int len) throws IOException {
            while (!closed && chunks.isEmpty()) {
                try { wait(); } catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new IOException(e); }
            }
            if (closed) return -1;
            byte[] chunk = chunks.remove(); int n = Math.min(chunk.length, len);
            System.arraycopy(chunk, 0, b, off, n);
            if (n < chunk.length) chunks.addFirst(Arrays.copyOfRange(chunk, n, chunk.length));
            return n;
        }
        @Override public synchronized void close() { closed = true; notifyAll(); }
    }

    private static final class FakeSocket implements RelayEngine.Duplex {
        final Pipe in = new Pipe();
        final ByteArrayOutputStream written = new ByteArrayOutputStream();
        final java.util.concurrent.atomic.AtomicInteger writeCalls = new java.util.concurrent.atomic.AtomicInteger();
        final CountDownLatch writeEntered = new CountDownLatch(1), connectEntered = new CountDownLatch(1);
        final CountDownLatch gate = new CountDownLatch(1), connectGate = new CountDownLatch(1);
        final CountDownLatch wasClosed = new CountDownLatch(1);
        volatile boolean closed, blockWrite, blockConnect;
        static void await(CountDownLatch latch) throws IOException {
            try { if (!latch.await(3, TimeUnit.SECONDS)) throw new IOException("Timeout teste"); }
            catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new IOException(e); }
        }
        @Override public void connect(String host, int port) throws IOException {
            assertEquals("api.openai.com", host); assertEquals(443, port);
            connectEntered.countDown(); if (blockConnect) await(connectGate);
            if (closed) throw new IOException("Cancelado");
        }
        @Override public InputStream input() { return in; }
        @Override public OutputStream output() {
            return new OutputStream() {
                @Override public void write(int b) throws IOException { write(new byte[]{(byte) b}); }
                @Override public void write(byte[] b) throws IOException { write(b, 0, b.length); }
                @Override public void write(byte[] b, int off, int len) throws IOException {
                    writeEntered.countDown(); if (blockWrite) await(gate);
                    if (closed) throw new IOException("Fechado");
                    writeCalls.incrementAndGet();
                    synchronized (written) { written.write(b, off, len); }
                }
            };
        }
        @Override public void close() { closed = true; in.close(); gate.countDown(); connectGate.countDown(); wasClosed.countDown(); }
    }

    private static final class Harness implements AutoCloseable {
        final ExecutorService pool = Executors.newFixedThreadPool(12);
        final BlockingQueue<Frame> sent = new LinkedBlockingQueue<>();
        final BlockingQueue<FakeSocket> sockets = new LinkedBlockingQueue<>();
        final List<FakeSocket> created = new CopyOnWriteArrayList<>();
        final AtomicLong clock = new AtomicLong(1000);
        final RelayEngine engine = new RelayEngine(new HostPolicy(HostPolicy.DEFAULTS), () -> {
            FakeSocket socket = sockets.poll(); if (socket == null) socket = new FakeSocket();
            created.add(socket); return socket;
        }, (bytes, valid) -> {
            if (valid.getAsBoolean()) sent.add(Frame.decode(bytes));
            return CompletableFuture.completedFuture(null);
        }, pool, clock::get);
        Frame take(int type) throws Exception {
            Frame f = sent.poll(3, TimeUnit.SECONDS); assertNotNull("Frame esperado tipo " + type, f); assertEquals(type, f.type); return f;
        }
        FakeSocket start(int id, int epoch) throws Exception {
            FakeSocket socket = new FakeSocket(); sockets.add(socket);
            engine.receive(open(id, epoch)); take(Frame.OPENED); return socket;
        }
        @Override public void close() throws Exception {
            engine.close(); pool.shutdownNow(); assertTrue(pool.awaitTermination(3, TimeUnit.SECONDS));
        }
    }

    @Test public void tlsFragmentationAndEightFrameWindowReleasedOnlyByEspAck() throws Exception {
        try (Harness h = new Harness()) {
            FakeSocket s = h.start(0, 7);
            byte[] original = new byte[173 * 8 + 97];
            for (int i = 0; i < original.length; i++) original[i] = (byte) i;
            s.in.feed(original);
            ByteArrayOutputStream joined = new ByteArrayOutputStream();
            for (int i = 0; i < 8; i++) {
                Frame f = h.take(Frame.DATA); assertEquals(i, f.seq); assertEquals(180, f.encode().length); joined.write(f.payload());
            }
            assertNull("Sem crédito, nenhum nono DATA", h.sent.poll(100, TimeUnit.MILLISECONDS));
            h.engine.receive(new Frame(Frame.ACK, 0, 7, 0, new byte[0]).encode());
            Frame tail = h.take(Frame.DATA); assertEquals(8, tail.seq); assertEquals(97, tail.payload().length);
            joined.write(tail.payload()); assertArrayEquals(original, joined.toByteArray());
        }
    }

    @Test public void ackOnlyAfterSocketOutputConsumedAndNoBleExecutorWait() throws Exception {
        try (Harness h = new Harness()) {
            FakeSocket s = h.start(0, 1); s.blockWrite = true;
            h.engine.receive(data(0, 1, 0)); assertTrue(s.writeEntered.await(2, TimeUnit.SECONDS));
            assertNull(h.sent.poll(100, TimeUnit.MILLISECONDS));
            // Outro canal continua operando enquanto o writer do primeiro está bloqueado.
            h.start(1, 1);
            s.gate.countDown(); Frame ack = h.take(Frame.ACK); assertEquals(0, ack.seq); assertEquals(0, ack.channel);
            synchronized (s.written) { assertArrayEquals(new byte[]{0}, s.written.toByteArray()); }
        }
    }

    @Test public void consecutiveQueuedBleFramesAreBatchedIntoTlsButAckedIndividually() throws Exception {
        try (Harness h = new Harness()) {
            FakeSocket s = h.start(0, 101); s.blockWrite = true;
            h.engine.receive(data(0, 101, 0)); assertTrue(s.writeEntered.await(2, TimeUnit.SECONDS));
            for (int i = 1; i < 8; i++) h.engine.receive(data(0, 101, i));
            s.gate.countDown();
            for (int i = 0; i < 8; i++) assertEquals(i, h.take(Frame.ACK).seq);
            synchronized (s.written) { assertArrayEquals(new byte[]{0,1,2,3,4,5,6,7}, s.written.toByteArray()); }
            assertEquals("one TLS write for frame 0 and one for the seven already queued", 2, s.writeCalls.get());
        }
    }

    @Test public void outOfSequenceDataAndWrongHeadAckCloseChannel() throws Exception {
        try (Harness h = new Harness()) {
            FakeSocket s = h.start(0, 2); h.engine.receive(data(0, 2, 1)); h.take(Frame.ERROR); assertTrue(s.closed);
            s = h.start(1, 2); s.in.feed(new byte[174]); h.take(Frame.DATA); h.take(Frame.DATA);
            h.engine.receive(new Frame(Frame.ACK, 1, 2, 1, new byte[0]).encode());
            h.take(Frame.ERROR); assertTrue(s.closed);
        }
    }

    @Test public void ninthQueuedInboundDataAbortsWithoutAckOrReplay() throws Exception {
        try (Harness h = new Harness()) {
            FakeSocket s = h.start(0, 3); s.blockWrite = true;
            h.engine.receive(data(0, 3, 0)); assertTrue(s.writeEntered.await(2, TimeUnit.SECONDS));
            for (int i = 1; i <= 8; i++) h.engine.receive(data(0, 3, i));
            h.take(Frame.ERROR); assertTrue(s.closed); assertNull(h.sent.poll(100, TimeUnit.MILLISECONDS));
        }
    }

    @Test public void newEpochCancelsOldSocketAndDropsLateFramesAndOpen() throws Exception {
        try (Harness h = new Harness()) {
            FakeSocket old = h.start(0, 4), current = h.start(0, 5); assertTrue(old.closed);
            h.engine.receive(data(0, 4, 0)); h.engine.receive(open(0, 4));
            h.engine.receive(new Frame(Frame.CLOSE, 0, 4, 0, new byte[0]).encode());
            assertNull(h.sent.poll(100, TimeUnit.MILLISECONDS)); assertFalse(current.closed);
            h.engine.receive(data(0, 5, 0)); assertEquals(5, h.take(Frame.ACK).epoch);
        }
    }

    @Test public void cancelDuringConnectCannotSendOpened() throws Exception {
        try (Harness h = new Harness()) {
            FakeSocket s = new FakeSocket(); s.blockConnect = true; h.sockets.add(s);
            h.engine.receive(open(0, 9)); assertTrue(s.connectEntered.await(2, TimeUnit.SECONDS));
            h.engine.receive(new Frame(Frame.CLOSE, 0, 9, 0, new byte[0]).encode()); h.take(Frame.CLOSE);
            assertTrue(s.closed); assertNull(h.sent.poll(100, TimeUnit.MILLISECONDS));
        }
    }

    @Test public void deadlinesAbortConnectWriteAndUnackedData() throws Exception {
        try (Harness h = new Harness()) {
            FakeSocket connecting = new FakeSocket(); connecting.blockConnect = true; h.sockets.add(connecting);
            h.engine.receive(open(0, 10)); assertTrue(connecting.connectEntered.await(2, TimeUnit.SECONDS));
            h.clock.addAndGet(20_000); h.engine.tick(); h.take(Frame.ERROR); assertTrue(connecting.closed);
            FakeSocket writing = h.start(1, 10); writing.blockWrite = true;
            h.engine.receive(data(1, 10, 0)); assertTrue(writing.writeEntered.await(2, TimeUnit.SECONDS));
            h.clock.addAndGet(20_000); h.engine.tick(); h.take(Frame.ERROR); assertTrue(writing.closed);
            FakeSocket reading = h.start(2, 10); reading.in.feed(new byte[]{1}); h.take(Frame.DATA);
            h.clock.addAndGet(20_000); h.engine.tick(); h.take(Frame.ERROR); assertTrue(reading.closed);
        }
    }

    @Test public void fourChannelsIndependentAndShutdownClosesAll() throws Exception {
        try (Harness h = new Harness()) {
            FakeSocket[] all = new FakeSocket[4];
            for (int i = 0; i < 4; i++) all[i] = h.start(i, 77);
            all[0].in.feed(new byte[173 * 8]); for (int i = 0; i < 8; i++) h.take(Frame.DATA);
            h.engine.receive(data(3, 77, 0)); assertEquals(3, h.take(Frame.ACK).channel);
            h.engine.close(); for (FakeSocket s : all) assertTrue(s.closed);
            h.engine.receive(open(0, 78)); assertNull(h.sent.poll(100, TimeUnit.MILLISECONDS));
        }
    }

    @Test public void disallowedOpenDoesNotCreateSocketAndMalformedFrameIsRejected() throws Exception {
        try (Harness h = new Harness()) {
            byte[] p = new byte[]{(byte) 0xbb, 1, 'e', 'v', 'i', 'l', '.', 'c', 'o', 'm'};
            h.engine.receive(new Frame(Frame.OPEN, 0, 1, 0, p).encode()); h.take(Frame.ERROR); assertTrue(h.created.isEmpty());
            assertThrows(IllegalArgumentException.class, () -> h.engine.receive(new byte[0]));
        }
    }

    @Test public void eofDrainsOutstandingDataBeforeClose() throws Exception {
        try (Harness h = new Harness()) {
            FakeSocket s = h.start(0, 40);
            s.in.feed(new byte[300]); h.take(Frame.DATA); h.take(Frame.DATA);
            s.in.close(); // EOF após os bytes que já foram encaminhados, mas ainda sem ACK.
            assertNull(h.sent.poll(100, TimeUnit.MILLISECONDS));
            h.engine.receive(new Frame(Frame.ACK, 0, 40, 0, new byte[0]).encode());
            assertNull(h.sent.poll(100, TimeUnit.MILLISECONDS));
            h.engine.receive(new Frame(Frame.ACK, 0, 40, 1, new byte[0]).encode());
            h.take(Frame.CLOSE); assertTrue(s.closed);
        }
    }

    private static final class QueuedBle {
        final Frame frame;
        final java.util.function.BooleanSupplier valid;
        final CompletableFuture<Void> completed = new CompletableFuture<>();
        QueuedBle(byte[] bytes, java.util.function.BooleanSupplier valid) { frame = Frame.decode(bytes); this.valid = valid; }
    }

    @Test public void epochChangeInvalidatesDataStillQueuedInBleAndLateFailureDoesNotCloseNewChannel() throws Exception {
        ExecutorService pool = Executors.newFixedThreadPool(4);
        BlockingQueue<QueuedBle> pending = new LinkedBlockingQueue<>();
        BlockingQueue<FakeSocket> sockets = new LinkedBlockingQueue<>();
        RelayEngine engine = new RelayEngine(new HostPolicy(HostPolicy.DEFAULTS), sockets::remove,
                (bytes, valid) -> { QueuedBle q = new QueuedBle(bytes, valid); pending.add(q); return q.completed; }, pool, () -> 1000L);
        try {
            FakeSocket old = new FakeSocket(); sockets.add(old); engine.receive(open(0, 50));
            QueuedBle opened = pending.poll(2, TimeUnit.SECONDS); assertNotNull(opened); assertTrue(opened.valid.getAsBoolean());
            opened.completed.complete(null); old.in.feed(new byte[]{1});
            QueuedBle oldData = pending.poll(2, TimeUnit.SECONDS); assertNotNull(oldData); assertEquals(Frame.DATA, oldData.frame.type);
            FakeSocket current = new FakeSocket(); sockets.add(current); engine.receive(open(0, 51));
            QueuedBle newOpened = pending.poll(2, TimeUnit.SECONDS); assertNotNull(newOpened);
            assertFalse(oldData.valid.getAsBoolean());
            oldData.completed.completeExceptionally(new java.util.concurrent.CancellationException());
            assertTrue(newOpened.valid.getAsBoolean()); newOpened.completed.complete(null);
            assertTrue(old.closed); assertFalse(current.closed);
            engine.receive(data(0, 51, 0));
            QueuedBle ack = pending.poll(2, TimeUnit.SECONDS); assertNotNull(ack); assertEquals(Frame.ACK, ack.frame.type); assertEquals(51, ack.frame.epoch);
        } finally {
            engine.close(); pool.shutdownNow(); assertTrue(pool.awaitTermination(3, TimeUnit.SECONDS));
        }
    }

    @Test public void inboundNotificationMayRaceOpenedGattCompletion() throws Exception {
        ExecutorService pool = Executors.newFixedThreadPool(4);
        BlockingQueue<QueuedBle> pending = new LinkedBlockingQueue<>();
        FakeSocket socket = new FakeSocket();
        RelayEngine engine = new RelayEngine(new HostPolicy(HostPolicy.DEFAULTS), () -> socket,
                (bytes, valid) -> { QueuedBle q = new QueuedBle(bytes, valid); pending.add(q); return q.completed; }, pool, () -> 1000L);
        try {
            engine.receive(open(0, 60)); QueuedBle opened = pending.poll(2, TimeUnit.SECONDS); assertNotNull(opened);
            // OPENED entregue ao ESP, porém callback Android ainda não executado.
            engine.receive(data(0, 60, 0));
            QueuedBle ack = pending.poll(2, TimeUnit.SECONDS); assertNotNull(ack); assertEquals(Frame.ACK, ack.frame.type);
            assertFalse(socket.closed); opened.completed.complete(null); ack.completed.complete(null);
        } finally {
            engine.close(); pool.shutdownNow(); assertTrue(pool.awaitTermination(3, TimeUnit.SECONDS));
        }
    }

    @Test public void legacyFunctionalFactoryRejectsVoiceEvenIfHostIsAllowlisted() throws Exception {
        ExecutorService pool = Executors.newFixedThreadPool(2); BlockingQueue<Frame> frames = new LinkedBlockingQueue<>();
        RelayEngine.SocketFactory legacy = () -> { fail("No DNS/TLS creation for reserved host"); return null; };
        RelayEngine engine = new RelayEngine(new HostPolicy(List.of(RelayEngine.VOICE_HOST)), legacy,
                (bytes, valid) -> { frames.add(Frame.decode(bytes)); return CompletableFuture.completedFuture(null); }, pool, () -> 1000L);
        try {
            assertFalse(legacy.supportsVoice()); assertThrows(IOException.class, legacy::createVoice);
            engine.receive(openHost(0, 1, RelayEngine.VOICE_HOST, 443));
            Frame error = frames.poll(2, TimeUnit.SECONDS); assertNotNull(error); assertEquals(Frame.ERROR, error.type);
        } finally { engine.close(); pool.shutdownNow(); assertTrue(pool.awaitTermination(3, TimeUnit.SECONDS)); }
    }

    @Test public void reservedLiteralOnlyBypassesPolicyAndUsesVoiceWithoutTlsFactory() throws Exception {
        ExecutorService pool = Executors.newFixedThreadPool(4); BlockingQueue<Frame> frames = new LinkedBlockingQueue<>();
        Pipe pipe = new Pipe(); ByteArrayOutputStream written = new ByteArrayOutputStream();
        java.util.concurrent.atomic.AtomicInteger created = new java.util.concurrent.atomic.AtomicInteger();
        RelayEngine.SocketFactory factory = new RelayEngine.SocketFactory() {
            @Override public RelayEngine.Duplex create() { fail("Reserved host must never reach DNS/TLS"); return null; }
            @Override public boolean supportsVoice() { return true; }
            @Override public RelayEngine.Duplex createVoice() {
                created.incrementAndGet();
                return new RelayEngine.Duplex() {
                    @Override public void connect(String host, int port) { assertEquals(RelayEngine.VOICE_HOST, host); assertEquals(443, port); }
                    @Override public InputStream input() { return pipe; }
                    @Override public OutputStream output() { return written; }
                    @Override public void close() { pipe.close(); }
                };
            }
        };
        RelayEngine engine = new RelayEngine(new HostPolicy(List.of(RelayEngine.VOICE_HOST)), factory,
                (bytes, valid) -> { if (valid.getAsBoolean()) frames.add(Frame.decode(bytes)); return CompletableFuture.completedFuture(null); }, pool, () -> 1000L);
        try {
            int epoch = 10;
            for (String near : List.of("VOICE-v1.ideiarec.invalid", RelayEngine.VOICE_HOST + ".", " " + RelayEngine.VOICE_HOST)) {
                engine.receive(openHost(0, epoch++, near, 443));
                Frame error = frames.poll(2, TimeUnit.SECONDS); assertNotNull(error); assertEquals(Frame.ERROR, error.type);
            }
            engine.receive(openHost(0, 100, RelayEngine.VOICE_HOST, 444));
            assertEquals(Frame.ERROR, frames.poll(2, TimeUnit.SECONDS).type); assertEquals(0, created.get());
            engine.receive(openHost(0, 101, RelayEngine.VOICE_HOST, 443));
            Frame opened = frames.poll(2, TimeUnit.SECONDS); assertNotNull(opened); assertEquals(Frame.OPENED, opened.type); assertEquals(0, opened.payload().length);
            assertEquals(1, created.get()); engine.receive(data(0, 101, 0));
            Frame ack = frames.poll(2, TimeUnit.SECONDS); assertNotNull(ack); assertEquals(Frame.ACK, ack.type);
            assertArrayEquals(new byte[]{0}, written.toByteArray());
        } finally { engine.close(); pool.shutdownNow(); assertTrue(pool.awaitTermination(3, TimeUnit.SECONDS)); }
    }
}
