package br.com.ideiarec.companion.voice;

import br.com.ideiarec.companion.protocol.RelayEngine;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.Base64;
import java.util.concurrent.locks.ReentrantLock;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** Per-channel Live session. HELLO/key are transient and never persisted or logged. */
public final class VoiceDuplex implements RelayEngine.Duplex {
    public interface Codec extends AutoCloseable {
        int lookahead() throws IOException;
        byte[] encode(short[] pcm) throws IOException;
        short[] decode(byte[] packet) throws IOException;
        void resetEncoder() throws IOException;
        void resetDecoder() throws IOException;
        @Override void close();
    }
    @FunctionalInterface public interface CodecFactory { Codec create(int bitrate) throws IOException; }
    public interface Listener {
        void message(String json) throws IOException;
        void ended(boolean normal);
    }
    public interface LiveSocket extends AutoCloseable {
        void connect(String key, Listener listener) throws IOException;
        void send(String json) throws IOException;
        @Override void close();
    }

    private static final int QUEUE_LIMIT = VoicePacket.MAX_PAYLOAD + VoicePacket.HEADER;
    private static final long QUEUE_TIMEOUT_MS = 15000;
    private final LiveSocket socket;
    private final CodecFactory codecs;
    private final Object lifecycle = new Object(), queueLock = new Object();
    private final ReentrantLock incoming = new ReentrantLock();
    private final VoicePacket.Decoder parser = new VoicePacket.Decoder();
    private final ArrayDeque<byte[]> queue = new ArrayDeque<>();
    private int queuedBytes, queueOffset;
    private volatile boolean closed, connected, eof;
    private volatile boolean turnCompleted;
    private Codec codec;
    private boolean hello, opus, inputEnded, inputStarted, resetOutput = true;
    private long inputDelivered, outputDelivered, segmentReal, encodedCursor;
    private int outputLookahead, partialCount;
    private final short[] partial = new short[480];

    public VoiceDuplex() { this(new GeminiWebSocket(), OpusCodec::new); }
    public VoiceDuplex(LiveSocket socket, CodecFactory codecs) { this.socket = socket; this.codecs = codecs; }

    @Override public void connect(String hostname, int port) throws IOException {
        synchronized (lifecycle) {
            if (closed || connected || !RelayEngine.VOICE_HOST.equals(hostname) || port != 443) throw invalid();
            connected = true; // Local OPEN only. No DNS/TLS until authenticated HELLO is parsed.
        }
    }
    private static IOException invalid() { return new IOException("Canal de voz encerrado"); }
    private void ready() throws IOException { if (closed || !connected) throw invalid(); }

    @Override public InputStream input() throws IOException {
        ready();
        return new InputStream() {
            @Override public int read() throws IOException { byte[] b = new byte[1]; return read(b) < 0 ? -1 : b[0] & 255; }
            @Override public int read(byte[] b, int off, int len) throws IOException {
                java.util.Objects.checkFromIndexSize(off, len, b.length);
                if (len == 0) return 0;
                synchronized (queueLock) {
                    while (queue.isEmpty() && !closed && !eof) {
                        try { queueLock.wait(QUEUE_TIMEOUT_MS); }
                        catch (InterruptedException e) { Thread.currentThread().interrupt(); throw invalid(); }
                    }
                    if (closed) throw invalid();
                    if (queue.isEmpty() && eof) return -1;
                    int total = 0;
                    // Pack only bytes already queued; never wait to fill a relay DATA frame.
                    while (total < len && !queue.isEmpty()) {
                        byte[] head = queue.element(); int n = Math.min(len - total, head.length - queueOffset);
                        System.arraycopy(head, queueOffset, b, off + total, n);
                        queueOffset += n; queuedBytes -= n; total += n;
                        if (queueOffset == head.length) { queue.remove(); queueOffset = 0; }
                    }
                    queueLock.notifyAll(); return total;
                }
            }
            @Override public void close() { VoiceDuplex.this.close(); }
        };
    }

    @Override public OutputStream output() throws IOException {
        ready();
        return new OutputStream() {
            @Override public void write(int b) throws IOException { write(new byte[]{(byte) b}); }
            @Override public void write(byte[] b, int off, int len) throws IOException {
                incoming.lock();
                try {
                    ready(); if (eof) throw invalid();
                    parser.accept(b, off, len, VoiceDuplex.this::receive);
                } catch (IOException | RuntimeException e) { VoiceDuplex.this.close(); throw invalid(); }
                finally { if (closed) parser.clear(); incoming.unlock(); }
            }
            @Override public void close() { VoiceDuplex.this.close(); }
        };
    }

    private void enqueue(VoicePacket packet) throws IOException {
        byte[] bytes = packet.encode();
        long deadline = System.nanoTime() + QUEUE_TIMEOUT_MS * 1000000;
        synchronized (queueLock) {
            while (!closed && queuedBytes + bytes.length > QUEUE_LIMIT) {
                long left = deadline - System.nanoTime();
                if (left <= 0) throw invalid();
                try { queueLock.wait(Math.max(1, left / 1000000)); }
                catch (InterruptedException e) { Thread.currentThread().interrupt(); throw invalid(); }
            }
            ready(); if (eof) throw invalid();
            queue.add(bytes); queuedBytes += bytes.length; queueLock.notifyAll();
        }
    }

    private void receive(VoicePacket packet) throws IOException {
        try {
            ready();
            if (!hello) { negotiate(packet); return; }
            switch (packet.type) {
                case VoicePacket.JSON:
                    if (packet.position != 0) throw invalid();
                    String json = VoicePacket.utf8(packet.payload);
                    JSONObject control = parseJson(json);
                    JSONObject realtime = control.optJSONObject("realtimeInput");
                    if (realtime != null && realtime.has("activityEnd")) throw invalid();
                    if (control.has("toolResponse") || (realtime != null && realtime.has("activityStart"))) turnCompleted = false;
                    socket.send(json); // No reserialization of setup, activityStart or toolResponse.
                    break;
                case VoicePacket.PCM:
                    if (opus || inputEnded || packet.position != inputDelivered) throw invalid();
                    sendPcm(packet.payload); inputDelivered += packet.payload.length / 2;
                    break;
                case VoicePacket.OPUS:
                    if (!opus || inputEnded || packet.position != inputDelivered) throw invalid();
                    if (!inputStarted && packet.flags != VoicePacket.RESET_DECODER) throw invalid();
                    if (packet.flags == VoicePacket.RESET_DECODER) codec.resetDecoder();
                    inputStarted = true;
                    int skip = (packet.payload[0] & 255) | ((packet.payload[1] & 255) << 8);
                    int take = (packet.payload[2] & 255) | ((packet.payload[3] & 255) << 8);
                    if (skip + take > 320) throw invalid();
                    short[] samples = codec.decode(Arrays.copyOfRange(packet.payload, 4, packet.payload.length));
                    if (samples.length != 320) throw invalid(); // Decode even zero-take priming/tail packets.
                    byte[] pcm = new byte[take * 2];
                    ByteBuffer little = ByteBuffer.wrap(pcm).order(ByteOrder.LITTLE_ENDIAN);
                    for (int i = skip; i < skip + take; i++) little.putShort(samples[i]);
                    if (take != 0) sendPcm(pcm);
                    inputDelivered += take;
                    break;
                case VoicePacket.END_INPUT:
                    if (inputEnded || packet.position != inputDelivered) throw invalid();
                    inputEnded = true; socket.send("{\"realtimeInput\":{\"activityEnd\":{}}}");
                    break;
                default: throw invalid();
            }
            if (inputDelivered > 0xffffffffL) throw invalid();
        } catch (JSONException e) { throw invalid(); }
        finally { if (packet.type == VoicePacket.HELLO) Arrays.fill(packet.payload, (byte) 0); }
    }

    private static int exactInt(JSONObject object, String key) throws JSONException, IOException {
        Object value = object.get(key);
        if (!(value instanceof Integer)) throw invalid();
        return (Integer) value;
    }

    private static JSONObject parseJson(String json) throws IOException, JSONException {
        int depth = 0; boolean quoted = false, escaped = false;
        for (int i = 0; i < json.length(); i++) {
            char c = json.charAt(i);
            if (quoted) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') quoted = false;
            } else if (c == '"') quoted = true;
            else if (c == '{' || c == '[') { if (++depth > 64) throw invalid(); }
            else if (c == '}' || c == ']') { if (--depth < 0) throw invalid(); }
        }
        if (depth != 0 || quoted) throw invalid();
        return new JSONObject(json);
    }

    private void negotiate(VoicePacket packet) throws JSONException, IOException {
        if (packet.type != VoicePacket.HELLO || packet.position != 0) throw invalid();
        JSONObject h = parseJson(VoicePacket.utf8(packet.payload));
        int bitrate = exactInt(h, "bitrate"), inputLookahead = exactInt(h, "inputLookahead");
        if (!(h.get("codec") instanceof String) || !(h.get("key") instanceof String)) throw invalid();
        String name = h.getString("codec"), key = h.getString("key");
        if (exactInt(h, "version") != 1 || exactInt(h, "inputRate") != 16000
                || exactInt(h, "outputRate") != 24000 || exactInt(h, "frameMs") != 20
                || (name.equals("opus") ? (bitrate != 64000 && bitrate != 48000 && bitrate != 32000) : bitrate != 0)
                || (!name.equals("opus") && !name.equals("pcm")) || inputLookahead < 0 || inputLookahead > 3200
                || (name.equals("pcm") && inputLookahead != 0) || key.isEmpty() || key.length() > 512) throw invalid();
        for (int i = 0; i < key.length(); i++) if (key.charAt(i) < 33 || key.charAt(i) > 126) throw invalid();
        opus = name.equals("opus");
        if (opus) {
            Codec created = codecs.create(bitrate);
            synchronized (lifecycle) {
                if (closed) { created.close(); throw invalid(); }
                codec = created;
            }
            outputLookahead = codec.lookahead();
            if (outputLookahead < 0 || outputLookahead > 4800) throw invalid();
        }
        JSONObject response = new JSONObject().put("version", 1).put("codec", name).put("bitrate", bitrate)
                .put("inputRate", 16000).put("outputRate", 24000).put("frameMs", 20).put("outputLookahead", outputLookahead);
        hello = true;
        enqueue(new VoicePacket(VoicePacket.HELLO, 0, 0, response.toString().getBytes(StandardCharsets.UTF_8)));
        socket.connect(key, new Listener() {
            @Override public void message(String json) throws IOException {
                try { serverJson(json); }
                catch (IOException | RuntimeException e) { VoiceDuplex.this.close(); throw invalid(); }
            }
            @Override public void ended(boolean normal) {
                if (!normal || !turnCompleted) { VoiceDuplex.this.close(); return; }
                Codec release;
                synchronized (lifecycle) {
                    if (closed || eof) return;
                    eof = true; release = codec; codec = null;
                }
                socket.close();
                if (release != null) release.close();
                synchronized (queueLock) { queueLock.notifyAll(); }
            }
        });
    }

    private void sendPcm(byte[] pcm) throws IOException {
        for (int offset = 0; offset < pcm.length; offset += 32000) {
            byte[] chunk = Arrays.copyOfRange(pcm, offset, Math.min(pcm.length, offset + 32000));
            socket.send("{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\""
                    + Base64.getEncoder().encodeToString(chunk) + "\"}}}");
        }
    }

    private void serverJson(String json) throws IOException {
        ready();
        if (json.getBytes(StandardCharsets.UTF_8).length > VoicePacket.MAX_PAYLOAD) throw invalid();
        try {
            JSONObject root = parseJson(json);
            if (root.has("error")) throw invalid(); // Never echo provider diagnostics (can contain request secrets).
            JSONObject content = root.optJSONObject("serverContent");
            boolean changed = false;
            if (content != null) {
                JSONObject turn = content.optJSONObject("modelTurn");
                if (turn != null) turnCompleted = false;
                JSONArray parts = turn == null ? null : turn.optJSONArray("parts");
                if (parts != null) for (int i = 0; i < parts.length(); i++) {
                    JSONObject part = parts.getJSONObject(i), inline = part.optJSONObject("inlineData");
                    if (inline == null) continue;
                    String mime = inline.optString("mimeType");
                    if (!mime.startsWith("audio/")) continue;
                    if (!mime.equals("audio/pcm;rate=24000")) throw invalid();
                    byte[] pcm;
                    try { pcm = Base64.getDecoder().decode(inline.getString("data")); }
                    catch (IllegalArgumentException e) { throw invalid(); }
                    if (pcm.length > VoicePacket.MAX_PCM || (pcm.length & 1) != 0) throw invalid();
                    outputAudio(pcm); part.remove("inlineData"); changed = true;
                }
                if (content.optBoolean("turnComplete")) { flushSegment(); turnCompleted = true; }
            }
            byte[] bytes = (changed ? root.toString() : json).getBytes(StandardCharsets.UTF_8);
            enqueue(new VoicePacket(VoicePacket.JSON, 0, 0, bytes));
        } catch (JSONException e) { throw invalid(); }
    }

    private void outputAudio(byte[] pcm) throws IOException {
        if (pcm.length == 0) return;
        turnCompleted = false;
        if (!opus) {
            enqueue(new VoicePacket(VoicePacket.PCM, 0, outputDelivered, pcm));
            outputDelivered += pcm.length / 2; return;
        }
        ByteBuffer bytes = ByteBuffer.wrap(pcm).order(ByteOrder.LITTLE_ENDIAN);
        while (bytes.hasRemaining()) {
            partial[partialCount++] = bytes.getShort(); segmentReal++;
            if (partialCount == 480) emitOpusFrame();
        }
    }

    private void emitOpusFrame() throws IOException {
        byte[] raw = codec.encode(partial);
        int skip = (int) Math.min(480, Math.max(0, outputLookahead - encodedCursor));
        long start = Math.max(0, encodedCursor - outputLookahead);
        int take = (int) Math.max(0, Math.min(480 - skip, Math.min(segmentReal, encodedCursor + 480 - outputLookahead) - start));
        byte[] payload = ByteBuffer.allocate(4 + raw.length).order(ByteOrder.LITTLE_ENDIAN)
                .putShort((short) skip).putShort((short) take).put(raw).array();
        enqueue(new VoicePacket(VoicePacket.OPUS, resetOutput ? VoicePacket.RESET_DECODER : 0, outputDelivered, payload));
        outputDelivered += take; encodedCursor += 480; partialCount = 0; resetOutput = false;
        Arrays.fill(partial, (short) 0);
    }

    private void flushSegment() throws IOException {
        if (!opus || segmentReal == 0) return;
        while (encodedCursor < segmentReal + outputLookahead) emitOpusFrame();
        codec.resetEncoder(); encodedCursor = 0; segmentReal = 0; partialCount = 0; resetOutput = true;
    }

    @Override public void close() {
        Codec release;
        synchronized (lifecycle) {
            if (closed) return;
            closed = true; release = codec; codec = null;
        }
        synchronized (queueLock) { queue.clear(); queuedBytes = 0; queueOffset = 0; queueLock.notifyAll(); }
        socket.close(); // Interrupt network I/O before waiting for any bounded native codec operation.
        if (release != null) release.close();
        if (incoming.tryLock()) {
            try { parser.clear(); } finally { incoming.unlock(); }
        } // An active writer clears in finally after network I/O has been interrupted.
    }
}
