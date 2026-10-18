package br.com.ideiarec.companion.voice;

import static org.junit.Assert.*;
import br.com.ideiarec.companion.protocol.RelayEngine;
import br.com.ideiarec.companion.protocol.Frame;
import br.com.ideiarec.companion.protocol.HostPolicy;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.List;
import java.util.ArrayDeque;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;
import org.junit.Test;

public class VoiceDuplexTest {
    private static final class Socket implements VoiceDuplex.LiveSocket {
        final List<String> sent = new ArrayList<>();
        VoiceDuplex.Listener listener;
        int connects; boolean closed;
        @Override public void connect(String key, VoiceDuplex.Listener listener) { assertEquals("test-key", key); connects++; this.listener = listener; }
        @Override public void send(String json) throws IOException { if (closed) throw new IOException(); sent.add(json); }
        @Override public void close() { closed = true; }
        void response(String json) throws IOException { listener.message(json); }
    }
    private static final class Codec implements VoiceDuplex.Codec {
        int decodes, encoderResets, decoderResets; boolean closed;
        final List<short[]> encoded = new ArrayList<>();
        @Override public int lookahead() { return 156; }
        @Override public byte[] encode(short[] pcm) { encoded.add(pcm.clone()); return new byte[]{42}; }
        @Override public short[] decode(byte[] packet) { decodes++; short[] r = new short[320]; for (short i = 0; i < 320; i++) r[i] = i; return r; }
        @Override public void resetEncoder() { encoderResets++; }
        @Override public void resetDecoder() { decoderResets++; }
        @Override public void close() { closed = true; }
    }
    private static final class Harness implements AutoCloseable {
        final Socket socket = new Socket(); final Codec codec = new Codec();
        final VoiceDuplex duplex = new VoiceDuplex(socket, bitrate -> codec);
        Harness() throws IOException { this(true); }
        Harness(boolean connect) throws IOException { if (connect) duplex.connect(RelayEngine.VOICE_HOST, 443); }
        void hello(String codec) throws Exception { write(VoicePacket.HELLO, 0, 0, helloJson(codec).getBytes(StandardCharsets.UTF_8)); }
        void write(int type, int flags, long position, byte[] payload) throws IOException {
            duplex.output().write(new VoicePacket(type, flags, position, payload).encode());
        }
        VoicePacket read() throws IOException {
            InputStream in = duplex.input();
            byte[] h = in.readNBytes(12); assertEquals(12, h.length);
            int size = ByteBuffer.wrap(h).order(ByteOrder.LITTLE_ENDIAN).getInt(4);
            byte[] body = in.readNBytes(size);
            List<VoicePacket> packets = new ArrayList<>(); VoicePacket.Decoder decoder = new VoicePacket.Decoder();
            decoder.accept(h, 0, h.length, packets::add); decoder.accept(body, 0, body.length, packets::add);
            assertEquals(1, packets.size()); return packets.get(0);
        }
        @Override public void close() { duplex.close(); }
    }
    private static String helloJson(String codec) {
        return "{\"version\":1,\"codec\":\"" + codec + "\",\"bitrate\":" + (codec.equals("opus") ? 64000 : 0)
                + ",\"inputRate\":16000,\"outputRate\":24000,\"frameMs\":20,\"key\":\"test-key\",\"inputLookahead\":" + (codec.equals("opus") ? 104 : 0) + "}";
    }
    private static byte[] opus(int skip, int take) {
        return ByteBuffer.allocate(5).order(ByteOrder.LITTLE_ENDIAN).putShort((short) skip).putShort((short) take).put((byte) 42).array();
    }
    private static String audio(byte[] pcm, boolean complete) {
        return "{\"serverContent\":{\"modelTurn\":{\"parts\":[{\"text\":\"preserve\",\"inlineData\":{\"mimeType\":\"audio/pcm;rate=24000\",\"data\":\""
                + Base64.getEncoder().encodeToString(pcm) + "\"}}]},\"turnComplete\":" + complete + "},\"toolCallCancellation\":{\"ids\":[\"x\"]}}";
    }

    @Test public void openIsLocalHelloHasNoSecretAndControlJsonIsUnchanged() throws Exception {
        try (Harness h = new Harness()) {
            assertEquals(0, h.socket.connects); h.hello("pcm");
            VoicePacket hello = h.read(); assertEquals(VoicePacket.HELLO, hello.type);
            JSONObject response = new JSONObject(VoicePacket.utf8(hello.payload));
            assertFalse(response.has("key")); assertEquals(0, response.getInt("outputLookahead"));
            String setup = "{ \"setup\":{\"model\":\"models/test\"} }";
            h.write(2, 0, 0, setup.getBytes(StandardCharsets.UTF_8));
            String tool = "{\"toolResponse\":{\"functionResponses\":[{\"id\":\"x\",\"response\":{\"ok\":true}}]}}";
            h.write(2, 0, 0, tool.getBytes(StandardCharsets.UTF_8));
            assertEquals(Arrays.asList(setup, tool), h.socket.sent);
        }
    }
    @Test public void pcmInputHasCorrectRatePositionAndActivityEndOnlyAfterDeliveredTotal() throws Exception {
        try (Harness h = new Harness()) {
            h.hello("pcm"); h.read();
            byte[] pcm = new byte[]{1, 0, 2, 0}; h.write(4, 0, 0, pcm); h.write(5, 0, 2, new byte[0]);
            JSONObject a = new JSONObject(h.socket.sent.get(0)).getJSONObject("realtimeInput").getJSONObject("audio");
            assertEquals("audio/pcm;rate=16000", a.getString("mimeType")); assertArrayEquals(pcm, Base64.getDecoder().decode(a.getString("data")));
            assertEquals("{\"realtimeInput\":{\"activityEnd\":{}}}", h.socket.sent.get(1));
        }
        try (Harness h = new Harness()) {
            h.hello("pcm"); assertThrows(IOException.class, () -> h.write(5, 0, 1, new byte[0])); assertTrue(h.socket.closed);
        }
    }
    @Test public void maxPcmInputIsChunkedBelowWebsocketLimit() throws Exception {
        try (Harness h = new Harness()) {
            h.hello("pcm"); byte[] pcm = new byte[VoicePacket.MAX_PCM]; h.write(4, 0, 0, pcm);
            int bytes = 0;
            for (String json : h.socket.sent) {
                assertTrue(json.length() <= VoicePacket.MAX_PAYLOAD);
                bytes += Base64.getDecoder().decode(new JSONObject(json).getJSONObject("realtimeInput").getJSONObject("audio").getString("data")).length;
            }
            assertEquals(pcm.length, bytes);
        }
    }
    @Test public void opusDecodeAlwaysConsumesPrimingAndTailButOnlyForwardsTrimmedSamples() throws Exception {
        try (Harness h = new Harness()) {
            h.hello("opus"); h.read();
            h.write(3, 1, 0, opus(320, 0)); h.write(3, 0, 0, opus(104, 216)); h.write(3, 0, 216, opus(0, 0));
            assertEquals(3, h.codec.decodes); assertEquals(1, h.codec.decoderResets); assertEquals(1, h.socket.sent.size());
            byte[] pcm = Base64.getDecoder().decode(new JSONObject(h.socket.sent.get(0)).getJSONObject("realtimeInput").getJSONObject("audio").getString("data"));
            assertEquals(432, pcm.length); assertEquals(104, ByteBuffer.wrap(pcm).order(ByteOrder.LITTLE_ENDIAN).getShort());
            h.write(5, 0, 216, new byte[0]);
        }
    }
    @Test public void invalidProfileTrimmingPositionAndMissingResetFailClosed() throws Exception {
        try (Harness h = new Harness()) {
            String hello = helloJson("opus").replace("24000", "48000");
            assertThrows(IOException.class, () -> h.write(1, 0, 0, hello.getBytes(StandardCharsets.UTF_8)));
            assertEquals(0, h.socket.connects);
        }
        for (String profile : new String[]{helloJson("pcm").replace("\"bitrate\":0", "\"bitrate\":64000"),
                helloJson("opus").replace("64000", "0")}) {
            try (Harness h = new Harness()) {
                assertThrows(IOException.class, () -> h.write(1, 0, 0, profile.getBytes(StandardCharsets.UTF_8)));
                assertEquals(0, h.socket.connects);
            }
        }
        for (int mode = 0; mode < 3; mode++) {
            try (Harness h = new Harness()) {
                h.hello("opus"); final int test = mode;
                assertThrows(IOException.class, () -> h.write(3, test == 0 ? 0 : 1, test == 1 ? 1 : 0, opus(200, test == 2 ? 121 : 120)));
                assertTrue(h.socket.closed); assertTrue(h.codec.closed);
            }
        }
    }
    @Test public void deepJsonAndSecretHeaderInjectionFailWithoutForwarding() throws Exception {
        try (Harness h = new Harness()) {
            String injected = helloJson("pcm").replace("test-key", "test-key\\r\\nx-injected:1");
            assertThrows(IOException.class, () -> h.write(1, 0, 0, injected.getBytes(StandardCharsets.UTF_8)));
            assertEquals(0, h.socket.connects);
        }
        try (Harness h = new Harness()) {
            h.hello("pcm"); String nested = "{\"nested\":" + "[".repeat(65) + "0" + "]".repeat(65) + "}";
            assertThrows(IOException.class, () -> h.write(2, 0, 0, nested.getBytes(StandardCharsets.UTF_8)));
            assertTrue(h.socket.sent.isEmpty()); assertTrue(h.socket.closed);
        }
    }
    @Test public void audioPrecedesJsonTailFlushPrecedesCompleteAndToolSegmentResetsAreExplicit() throws Exception {
        try (Harness h = new Harness()) {
            h.hello("opus"); assertEquals(156, new JSONObject(VoicePacket.utf8(h.read().payload)).getInt("outputLookahead"));
            byte[] pcm = new byte[1000]; for (int i = 0; i < pcm.length; i++) pcm[i] = (byte) i;
            h.socket.response(audio(Arrays.copyOfRange(pcm, 0, 600), false));
            assertEquals(VoicePacket.JSON, h.read().type); assertEquals(0, h.codec.encoded.size());
            h.socket.response(audio(Arrays.copyOfRange(pcm, 600, 1000), true));
            VoicePacket a = h.read(), b = h.read(), complete = h.read();
            assertEquals(3, a.type); assertEquals(1, a.flags); assertEquals(0, a.position);
            ByteBuffer trim = ByteBuffer.wrap(a.payload).order(ByteOrder.LITTLE_ENDIAN); assertEquals(156, trim.getShort()); assertEquals(324, trim.getShort());
            assertEquals(324, b.position); trim = ByteBuffer.wrap(b.payload).order(ByteOrder.LITTLE_ENDIAN); assertEquals(0, trim.getShort()); assertEquals(176, trim.getShort());
            JSONObject control = new JSONObject(VoicePacket.utf8(complete.payload));
            assertTrue(control.has("toolCallCancellation")); assertTrue(control.getJSONObject("serverContent").getBoolean("turnComplete"));
            JSONObject part = control.getJSONObject("serverContent").getJSONObject("modelTurn").getJSONArray("parts").getJSONObject(0);
            assertEquals("preserve", part.getString("text")); assertFalse(part.has("inlineData")); assertEquals(1, h.codec.encoderResets);
            h.socket.response(audio(new byte[2], true));
            VoicePacket next = h.read(); assertEquals(1, next.flags); assertEquals(500, next.position); assertEquals(2, h.codec.encoderResets);
            assertEquals(0, h.codec.decoderResets);
        }
    }
    @Test public void pcmOutputIsNotDuplicatedAndUnknownControlFieldsRemain() throws Exception {
        try (Harness h = new Harness()) {
            h.hello("pcm"); h.read(); h.socket.response(audio(new byte[]{5, 0}, true));
            VoicePacket pcm = h.read(); assertEquals(4, pcm.type); assertArrayEquals(new byte[]{5, 0}, pcm.payload);
            assertFalse(VoicePacket.utf8(h.read().payload).contains("inlineData"));
            String control = "{ \"toolCall\":{\"functionCalls\":[]}, \"goAway\":{\"timeLeft\":\"1s\"} }";
            h.socket.response(control); assertEquals(control, VoicePacket.utf8(h.read().payload));
        }
    }
    @Test public void packedReadsCrossQueuedRecordsAndPreserveOffsetsBoundariesAndNormalEof() throws Exception {
        String first = "{\"a\":\"" + "x".repeat(116) + "\"}", second = first.replace("\"a\"", "\"b\"");
        String complete = "{\"serverContent\":{\"turnComplete\":true}}";
        ByteArrayOutputStream expected = new ByteArrayOutputStream();
        for (String json : List.of(first, second, complete)) {
            expected.write(new VoicePacket(VoicePacket.JSON, 0, 0, json.getBytes(StandardCharsets.UTF_8)).encode());
        }
        assertEquals(136, new VoicePacket(VoicePacket.JSON, 0, 0, first.getBytes(StandardCharsets.UTF_8)).encode().length);
        for (int size : new int[]{1, 11, 12, 13, 135, 136, 137, Frame.MAX_PAYLOAD, 1024}) {
            try (Harness h = new Harness()) {
                h.hello("pcm"); h.read(); InputStream in = h.duplex.input();
                h.socket.response(first); h.socket.response(second); h.socket.response(complete); h.socket.listener.ended(true);
                byte[] all = expected.toByteArray(), buffer = new byte[size + 6]; int at = 0;
                while (at < all.length) {
                    Arrays.fill(buffer, (byte) 0x55);
                    int count = in.read(buffer, 3, size);
                    assertEquals(Math.min(size, all.length - at), count);
                    assertArrayEquals(Arrays.copyOfRange(all, at, at + count), Arrays.copyOfRange(buffer, 3, 3 + count));
                    for (int i = 0; i < 3; i++) assertEquals(0x55, buffer[i]);
                    for (int i = 3 + count; i < buffer.length; i++) assertEquals(0x55, buffer[i]);
                    at += count;
                }
                assertEquals(-1, in.read(buffer)); assertEquals(-1, in.read()); assertEquals(0, in.read(buffer, 3, 0));
                assertThrows(IndexOutOfBoundsException.class, () -> in.read(buffer, -1, 1));
                assertThrows(IndexOutOfBoundsException.class, () -> in.read(buffer, 3, buffer.length));
            }
        }
    }
    @Test public void packedReadReturnsAvailableBytesWithoutWaitingForAnotherRecord() throws Exception {
        ExecutorService pool = Executors.newSingleThreadExecutor();
        try (Harness h = new Harness()) {
            h.hello("pcm"); h.read(); InputStream in = h.duplex.input();
            h.socket.response("{}"); byte[] bytes = new byte[Frame.MAX_PAYLOAD];
            Future<Integer> read = pool.submit(() -> in.read(bytes));
            assertEquals(Integer.valueOf(14), read.get(2, TimeUnit.SECONDS));
            assertArrayEquals(new VoicePacket(VoicePacket.JSON, 0, 0, "{}".getBytes(StandardCharsets.UTF_8)).encode(), Arrays.copyOf(bytes, 14));
            Future<?> waiting = pool.submit(() -> assertThrows(IOException.class, () -> in.read(bytes)));
            h.duplex.close(); waiting.get(2, TimeUnit.SECONDS);
        } finally { pool.shutdownNow(); assertTrue(pool.awaitTermination(3, TimeUnit.SECONDS)); }
    }
    @Test public void cancelAfterPackedReadDiscardsTheQueuedRemainderInsteadOfReturningIt() throws Exception {
        try (Harness h = new Harness()) {
            h.hello("pcm"); h.read(); InputStream in = h.duplex.input();
            String json = "{\"a\":\"" + "x".repeat(116) + "\"}";
            h.socket.response(json); h.socket.response(json);
            assertEquals(Frame.MAX_PAYLOAD, in.read(new byte[Frame.MAX_PAYLOAD]));
            h.duplex.close();
            assertThrows(IOException.class, () -> in.read(new byte[Frame.MAX_PAYLOAD]));
        }
    }
    @Test public void providerErrorIsGenericAndCloseUnblocksReaderAndQueueProducer() throws Exception {
        try (Harness h = new Harness()) {
            h.hello("opus");
            IOException failure = assertThrows(IOException.class, () -> h.socket.response("{\"error\":{\"message\":\"test-key secret\"}}"));
            assertFalse(failure.toString().contains("test-key")); assertTrue(h.socket.closed); assertTrue(h.codec.closed);
        }
        ExecutorService pool = Executors.newFixedThreadPool(2);
        try (Harness h = new Harness()) {
            Future<?> reader = pool.submit(() -> assertThrows(IOException.class, () -> h.duplex.input().read()));
            h.duplex.close(); reader.get(2, TimeUnit.SECONDS);
        } finally { pool.shutdownNow(); }
        pool = Executors.newSingleThreadExecutor();
        try (Harness h = new Harness()) {
            h.hello("pcm"); h.read();
            String padding = "x".repeat(190000);
            String json = "{\"padding\":\"" + padding + "\"}"; h.socket.response(json);
            CountDownLatch entered = new CountDownLatch(1);
            Future<?> producer = pool.submit(() -> { entered.countDown(); assertThrows(IOException.class, () -> h.socket.response(json)); });
            assertTrue(entered.await(2, TimeUnit.SECONDS)); h.duplex.close(); producer.get(2, TimeUnit.SECONDS);
        } finally { pool.shutdownNow(); }
    }
    @Test public void normalWebsocketCloseDrainsVoiceQueueAndRelayAcksBeforeClose() throws Exception {
        ExecutorService pool = Executors.newFixedThreadPool(4);
        BlockingQueue<Frame> sent = new LinkedBlockingQueue<>();
        try (Harness h = new Harness(false)) {
            RelayEngine.SocketFactory factory = new RelayEngine.SocketFactory() {
                @Override public RelayEngine.Duplex create() { fail("No legacy socket for voice"); return null; }
                @Override public boolean supportsVoice() { return true; }
                @Override public RelayEngine.Duplex createVoice() { return h.duplex; }
            };
            try (RelayEngine relay = new RelayEngine(new HostPolicy(List.of()), factory,
                    (bytes, valid) -> { if (valid.getAsBoolean()) sent.add(Frame.decode(bytes)); return CompletableFuture.completedFuture(null); }, pool, () -> 1000L)) {
                byte[] host = RelayEngine.VOICE_HOST.getBytes(StandardCharsets.UTF_8);
                byte[] open = ByteBuffer.allocate(host.length + 2).order(ByteOrder.LITTLE_ENDIAN).putShort((short) 443).put(host).array();
                relay.receive(new Frame(Frame.OPEN, 0, 1, 0, open).encode());
                Frame opened = sent.poll(2, TimeUnit.SECONDS); assertNotNull(opened); assertEquals(Frame.OPENED, opened.type);
                h.hello("pcm"); Frame hello = sent.poll(2, TimeUnit.SECONDS); assertNotNull(hello); assertEquals(Frame.DATA, hello.type);
                relay.receive(new Frame(Frame.ACK, 0, 1, hello.seq, new byte[0]).encode());
                byte[] original = new byte[4096]; for (int i = 0; i < original.length; i++) original[i] = (byte) i;
                h.socket.response(audio(original, true)); h.socket.listener.ended(true);
                assertTrue(h.socket.closed);
                ByteArrayOutputStream joined = new ByteArrayOutputStream(); ArrayDeque<Frame> unacked = new ArrayDeque<>();
                for (int i = 0; i < 8; i++) {
                    Frame data = sent.poll(2, TimeUnit.SECONDS); assertNotNull(data); assertEquals(Frame.DATA, data.type);
                    joined.write(data.payload()); unacked.add(data);
                }
                assertNull("EOF must not abort a full relay window", sent.poll(100, TimeUnit.MILLISECONDS));
                boolean closed = false;
                while (!unacked.isEmpty()) {
                    Frame ack = unacked.remove(); relay.receive(new Frame(Frame.ACK, 0, 1, ack.seq, new byte[0]).encode());
                    Frame next = sent.poll(100, TimeUnit.MILLISECONDS);
                    if (next == null) continue;
                    if (next.type == Frame.DATA) { joined.write(next.payload()); unacked.add(next); }
                    else { assertEquals(Frame.CLOSE, next.type); assertTrue(unacked.isEmpty()); closed = true; }
                }
                if (!closed) { Frame close = sent.poll(2, TimeUnit.SECONDS); assertNotNull(close); assertEquals(Frame.CLOSE, close.type); }
                List<VoicePacket> records = new ArrayList<>(); byte[] all = joined.toByteArray();
                new VoicePacket.Decoder().accept(all, 0, all.length, records::add);
                assertEquals(2, records.size()); assertEquals(VoicePacket.PCM, records.get(0).type);
                assertArrayEquals(original, records.get(0).payload);
                assertTrue(new JSONObject(VoicePacket.utf8(records.get(1).payload)).getJSONObject("serverContent").getBoolean("turnComplete"));
            }
        } finally { pool.shutdownNow(); assertTrue(pool.awaitTermination(3, TimeUnit.SECONDS)); }
    }
    @Test public void normalCloseBeforeTurnCompleteRemainsAnAbort() throws Exception {
        try (Harness h = new Harness()) {
            h.hello("opus"); h.socket.listener.ended(true);
            assertTrue(h.socket.closed); assertTrue(h.codec.closed);
            assertThrows(IOException.class, () -> h.duplex.input().read());
        }
    }
    @Test public void canceledPartialHelloIsZeroedWithoutRetainingSecret() throws Exception {
        try (Harness h = new Harness()) {
            byte[] encoded = new VoicePacket(VoicePacket.HELLO, 0, 0, helloJson("pcm").getBytes(StandardCharsets.UTF_8)).encode();
            h.duplex.output().write(encoded, 0, encoded.length - 1);
            java.lang.reflect.Field parserField = VoiceDuplex.class.getDeclaredField("parser"); parserField.setAccessible(true);
            Object parser = parserField.get(h.duplex);
            java.lang.reflect.Field bodyField = VoicePacket.Decoder.class.getDeclaredField("body"); bodyField.setAccessible(true);
            byte[] pending = (byte[]) bodyField.get(parser);
            assertNotNull(pending); assertTrue(new String(pending, StandardCharsets.UTF_8).contains("test-key"));
            h.duplex.close(); assertNull(bodyField.get(parser));
            assertArrayEquals(new byte[pending.length], pending); assertEquals(0, h.socket.connects);
        }
    }
    @Test public void newToolAudioAfterCompletedTurnCannotMasqueradeAsCleanEof() throws Exception {
        try (Harness h = new Harness()) {
            h.hello("opus"); h.socket.response(audio(new byte[2], true));
            h.socket.response(audio(new byte[2], false)); h.socket.listener.ended(true);
            assertTrue(h.codec.closed); assertThrows(IOException.class, () -> h.duplex.input().read());
        }
    }
}
