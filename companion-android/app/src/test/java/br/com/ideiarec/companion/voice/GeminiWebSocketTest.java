package br.com.ideiarec.companion.voice;

import static org.junit.Assert.*;
import java.io.BufferedOutputStream;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.Test;

public class GeminiWebSocketTest {
    private static final class Writes extends OutputStream {
        final ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        int single, bulk, flushes;
        @Override public void write(int b) { single++; bytes.write(b); }
        @Override public void write(byte[] b, int off, int len) { bulk++; bytes.write(b, off, len); }
        @Override public void flush() { flushes++; }
    }
    private static GeminiWebSocket writer(OutputStream out) throws Exception {
        GeminiWebSocket socket = new GeminiWebSocket();
        java.lang.reflect.Field output = GeminiWebSocket.class.getDeclaredField("output"); output.setAccessible(true);
        output.set(socket, new BufferedOutputStream(out, 4096));
        return socket;
    }
    private static void assertClientFrame(byte[] encoded, byte[] expected) throws IOException {
        ByteArrayInputStream in = new ByteArrayInputStream(encoded);
        assertEquals(0x81, in.read());
        int second = in.read(); assertEquals(128, second & 128);
        int marker = second & 127;
        assertEquals(expected.length < 126 ? expected.length : expected.length <= 65535 ? 126 : 127, marker);
        long length = marker;
        if (marker == 126 || marker == 127) {
            length = 0;
            for (int i = 0; i < (marker == 126 ? 2 : 8); i++) {
                int b = in.read(); assertTrue(b >= 0); length = (length << 8) | b;
            }
        }
        assertEquals(expected.length, length);
        byte[] mask = in.readNBytes(4); assertEquals(4, mask.length);
        byte[] decoded = in.readNBytes(expected.length); assertEquals(expected.length, decoded.length);
        for (int i = 0; i < decoded.length; i++) decoded[i] ^= mask[i & 3];
        assertArrayEquals(expected, decoded); assertEquals(-1, in.read());
    }
    @Test public void eachSmallFrameIsFlushedOnceAsOneTlsWriteWithoutBatchingFrames() throws Exception {
        Writes writes = new Writes(); GeminiWebSocket socket = writer(writes);
        try {
            int offset = 0, count = 0;
            for (String message : List.of("", "{}", "x".repeat(125), "x".repeat(126), "x".repeat(1000))) {
                socket.send(message); count++;
                assertEquals(count, writes.bulk); assertEquals(0, writes.single); assertEquals(count, writes.flushes);
                byte[] all = writes.bytes.toByteArray();
                assertClientFrame(Arrays.copyOfRange(all, offset, all.length), message.getBytes(StandardCharsets.UTF_8));
                offset = all.length;
            }
            socket.close(); assertEquals(count, writes.flushes);
            assertThrows(IOException.class, () -> socket.send("not sent"));
            assertEquals(count, writes.bulk);
        } finally { socket.close(); }
    }
    @Test public void bufferingPreservesMaskLengthsAndBytesAcrossBufferAndExtendedLengthBoundaries() throws Exception {
        for (int size : new int[]{4088, 4095, 4096, 4097, 65535, 65536, VoicePacket.MAX_PAYLOAD}) {
            Writes writes = new Writes(); GeminiWebSocket socket = writer(writes);
            try {
                String message = "0123456789abcdef".repeat((size + 15) / 16).substring(0, size);
                socket.send(message);
                assertEquals(1, writes.flushes); assertEquals(0, writes.single);
                assertClientFrame(writes.bytes.toByteArray(), message.getBytes(StandardCharsets.UTF_8));
            } finally { socket.close(); }
        }
    }
    @Test public void failedBufferedFlushAbortsWithoutRetryingOrFlushingOnClose() throws Exception {
        AtomicInteger writes = new AtomicInteger(), flushes = new AtomicInteger();
        GeminiWebSocket socket = writer(new OutputStream() {
            @Override public void write(int b) { fail("Buffered frame must use bulk writes"); }
            @Override public void write(byte[] b, int off, int len) throws IOException { writes.incrementAndGet(); throw new IOException("private failure"); }
            @Override public void flush() { flushes.incrementAndGet(); }
        });
        IOException failure = assertThrows(IOException.class, () -> socket.send("test-key secret"));
        assertFalse(failure.toString().contains("test-key")); assertFalse(failure.toString().contains("private"));
        socket.close(); assertThrows(IOException.class, () -> socket.send("retry"));
        assertEquals(1, writes.get()); assertEquals(0, flushes.get());
    }
    @Test public void cancelInterruptsBufferedFlushWithoutWaitingForTheWriterLock() throws Exception {
        CountDownLatch entered = new CountDownLatch(1), interrupted = new CountDownLatch(1);
        GeminiWebSocket socket = writer(new OutputStream() {
            @Override public void write(int b) { fail("Buffered frame must use bulk writes"); }
            @Override public void write(byte[] b, int off, int len) throws IOException {
                entered.countDown();
                try { if (!interrupted.await(3, TimeUnit.SECONDS)) throw new IOException("cancel did not interrupt write"); }
                catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new IOException(e); }
                throw new IOException("socket closed");
            }
        });
        java.lang.reflect.Field tcp = GeminiWebSocket.class.getDeclaredField("tcp"); tcp.setAccessible(true);
        tcp.set(socket, new Socket() { @Override public void close() { interrupted.countDown(); } });
        ExecutorService pool = Executors.newSingleThreadExecutor();
        try {
            Future<?> sending = pool.submit(() -> assertThrows(IOException.class, () -> socket.send("one frame")));
            assertTrue(entered.await(2, TimeUnit.SECONDS)); socket.close(); sending.get(2, TimeUnit.SECONDS);
            assertThrows(IOException.class, () -> socket.send("after cancel"));
        } finally { socket.close(); pool.shutdownNow(); assertTrue(pool.awaitTermination(3, TimeUnit.SECONDS)); }
    }
    private static byte[] frame(int opcode, boolean fin, byte[] payload) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream(); out.write((fin ? 128 : 0) | opcode);
        if (payload.length < 126) out.write(payload.length);
        else if (payload.length <= 65535) { out.write(126); out.write(payload.length >>> 8); out.write(payload.length); }
        else { out.write(127); for (int i = 56; i >= 0; i -= 8) out.write((int) ((long) payload.length >>> i)); }
        out.write(payload); return out.toByteArray();
    }
    private static final class Listener implements VoiceDuplex.Listener {
        final List<String> messages = new ArrayList<>();
        @Override public void message(String message) { messages.add(message); }
        @Override public void ended(boolean normal) { }
    }
    @Test public void verifiesRfcHandshakeAndRejectsRedirectExtensionsAndForgedAccept() throws Exception {
        String good = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
        String nonce = "dGhlIHNhbXBsZSBub25jZQ==";
        GeminiWebSocket.verifyUpgrade(good, nonce);
        assertThrows(IOException.class, () -> GeminiWebSocket.verifyUpgrade(good.replace("101", "302"), nonce));
        assertThrows(IOException.class, () -> GeminiWebSocket.verifyUpgrade(good, "wrong"));
        assertThrows(IOException.class, () -> GeminiWebSocket.verifyUpgrade(good.replace("\r\n\r\n", "\r\nSec-WebSocket-Extensions: permessage-deflate\r\n\r\n"), nonce));
    }
    @Test public void fragmentedJsonPingBinaryAndCloseRemainOrdered() throws Exception {
        ByteArrayOutputStream input = new ByteArrayOutputStream();
        input.write(frame(1, false, "{\"a\":".getBytes(StandardCharsets.UTF_8)));
        input.write(frame(9, true, new byte[]{1, 2})); input.write(frame(0, true, "1}".getBytes(StandardCharsets.UTF_8)));
        input.write(frame(2, true, "{}".getBytes(StandardCharsets.UTF_8))); input.write(frame(8, true, new byte[0]));
        Listener listener = new Listener(); List<byte[]> pongs = new ArrayList<>();
        GeminiWebSocket.readMessages(new ByteArrayInputStream(input.toByteArray()), listener, (opcode, bytes) -> { assertEquals(10, opcode); pongs.add(bytes); });
        assertEquals(List.of("{\"a\":1}", "{}"), listener.messages); assertArrayEquals(new byte[]{1, 2}, pongs.get(0));
    }
    @Test public void rejectsHugeFrameBeforeReadingAnyPayload() {
        byte[] header = new byte[]{(byte) 0x81, 127, 0, 0, 0, 0, 0, 4, 0, 1};
        ByteArrayInputStream input = new ByteArrayInputStream(header) {
            @Override public synchronized int read(byte[] bytes, int offset, int length) { fail("Oversize payload must not be read or allocated"); return -1; }
        };
        assertThrows(IOException.class, () -> GeminiWebSocket.readMessages(input, new Listener(), (opcode, bytes) -> fail()));
    }
    @Test public void rejectsCumulativeFragmentOverflowMaskedServerAndInvalidUtf8() throws Exception {
        ByteArrayOutputStream input = new ByteArrayOutputStream(); input.write(frame(1, false, new byte[262144])); input.write(frame(0, true, new byte[]{1}));
        assertThrows(IOException.class, () -> GeminiWebSocket.readMessages(new ByteArrayInputStream(input.toByteArray()), new Listener(), (opcode, bytes) -> fail()));
        for (byte[] invalid : List.of(new byte[]{(byte) 0x81, (byte) 0x80}, new byte[]{(byte) 0x80, 0},
                frame(1, true, new byte[]{(byte) 0xc0}), frame(9, false, new byte[0]))) {
            assertThrows(IOException.class, () -> GeminiWebSocket.readMessages(new ByteArrayInputStream(invalid), new Listener(), (opcode, bytes) -> fail()));
        }
    }
    @Test public void normalCloseIsDistinctFromErrorAndMissingStatus() throws Exception {
        for (int code : new int[]{1000, 1001, 1008, 1011}) {
            byte[] bytes = frame(8, true, new byte[]{(byte) (code >>> 8), (byte) code});
            assertEquals(code == 1000, GeminiWebSocket.readMessages(new ByteArrayInputStream(bytes), new Listener(), (opcode, payload) -> fail()));
        }
        assertFalse(GeminiWebSocket.readMessages(new ByteArrayInputStream(frame(8, true, new byte[0])), new Listener(), (opcode, payload) -> fail()));
    }
    @Test public void unreachableFirstAddressTriesNextWithinBudgetAndCancellationStopsRetries() throws Exception {
        InetAddress ipv6 = InetAddress.getByAddress(new byte[]{0x20, 1, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
        InetAddress ipv4 = InetAddress.getByAddress(new byte[]{8, 8, 8, 8});
        AtomicInteger attempts = new AtomicInteger();
        GeminiWebSocket socket = new GeminiWebSocket(() -> new Socket() {
            @Override public void connect(SocketAddress address, int timeout) throws IOException {
                assertTrue(timeout > 0 && timeout <= 3000);
                if (attempts.incrementAndGet() == 1) { assertEquals(ipv6, ((InetSocketAddress) address).getAddress()); throw new IOException(); }
                assertEquals(ipv4, ((InetSocketAddress) address).getAddress());
            }
        });
        try { assertNotNull(socket.connectAddresses(List.of(ipv6, ipv4), System.nanoTime() + TimeUnit.SECONDS.toNanos(15))); assertEquals(2, attempts.get()); }
        finally { socket.close(); }
        GeminiWebSocket[] canceled = new GeminiWebSocket[1]; attempts.set(0);
        canceled[0] = new GeminiWebSocket(() -> new Socket() {
            @Override public void connect(SocketAddress address, int timeout) throws IOException { attempts.incrementAndGet(); canceled[0].close(); throw new IOException(); }
        });
        assertThrows(IOException.class, () -> canceled[0].connectAddresses(List.of(ipv6, ipv4), System.nanoTime() + TimeUnit.SECONDS.toNanos(15)));
        assertEquals(1, attempts.get());
    }
}
