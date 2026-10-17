package br.com.ideiarec.companion.voice;

import static org.junit.Assert.*;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.SocketAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.Test;

public class GeminiWebSocketTest {
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
