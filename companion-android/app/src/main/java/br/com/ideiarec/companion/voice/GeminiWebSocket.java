package br.com.ideiarec.companion.voice;

import android.net.DnsResolver;
import android.os.CancellationSignal;
import br.com.ideiarec.companion.protocol.HostPolicy;
import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.InetAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.SecureRandom;
import java.util.Base64;
import java.util.Collections;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import javax.net.ssl.SNIHostName;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

/** Minimal RFC6455 client: fixed origin, no redirects/extensions, bounded before allocation.
 * API/auth: googleapis/python-genai google/genai/live.py + _api_client.py (x-goog-api-key).
 */
final class GeminiWebSocket implements VoiceDuplex.LiveSocket {
    static final String HOST = "generativelanguage.googleapis.com";
    static final String PATH = "/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent";
    private static final ScheduledExecutorService DEADLINES = Executors.newSingleThreadScheduledExecutor(r -> {
        Thread t = new Thread(r, "voice-deadline"); t.setDaemon(true); return t;
    });
    private final Object lifecycle = new Object(), writer = new Object();
    private final SecureRandom random = new SecureRandom();
    private Socket tcp;
    private SSLSocket tls;
    private CancellationSignal dnsCancellation;
    private final CompletableFuture<List<InetAddress>> addresses = new CompletableFuture<>();
    private volatile OutputStream output;
    private volatile boolean closed;
    private boolean started;
    @FunctionalInterface interface TcpFactory { Socket create() throws IOException; }
    private final TcpFactory tcpFactory;
    GeminiWebSocket() { this(Socket::new); }
    GeminiWebSocket(TcpFactory factory) { tcpFactory = factory; }

    @Override public void connect(String key, VoiceDuplex.Listener listener) throws IOException {
        synchronized (lifecycle) {
            if (closed || started) throw failure();
            started = true;
        }
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(15);
        ScheduledFuture<?> timeout = DEADLINES.schedule(this::close, 15, TimeUnit.SECONDS);
        try {
            Socket raw = connectAddresses(resolve(), deadline);
            raw.setSoTimeout(15000);
            SSLSocket secure = (SSLSocket) ((SSLSocketFactory) SSLSocketFactory.getDefault()).createSocket(raw, HOST, 443, true);
            synchronized (lifecycle) {
                if (closed) { secure.close(); throw failure(); }
                tls = secure;
            }
            SSLParameters parameters = secure.getSSLParameters();
            parameters.setEndpointIdentificationAlgorithm("HTTPS");
            parameters.setServerNames(Collections.singletonList(new SNIHostName(HOST)));
            secure.setSSLParameters(parameters); secure.setSoTimeout(15000); secure.startHandshake();
            InputStream in = secure.getInputStream(); OutputStream out = secure.getOutputStream();
            byte[] nonce = new byte[16]; random.nextBytes(nonce);
            String challenge = Base64.getEncoder().encodeToString(nonce);
            byte[] request = ("GET " + PATH + " HTTP/1.1\r\nHost: " + HOST
                    + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "
                    + challenge + "\r\nx-goog-api-key: " + key + "\r\n\r\n").getBytes(StandardCharsets.US_ASCII);
            try { out.write(request); out.flush(); }
            finally { java.util.Arrays.fill(request, (byte) 0); }
            verifyUpgrade(readHeaders(in), challenge);
            secure.setSoTimeout(90000);
            synchronized (lifecycle) { if (closed) throw failure(); output = out; }
            Thread reader = new Thread(() -> {
                boolean normal = false;
                try { normal = readMessages(in, listener); }
                catch (IOException | RuntimeException ignored) { /* Generic channel failure only. */ }
                finally { close(); listener.ended(normal); }
            }, "voice-gemini");
            reader.setDaemon(true); reader.start();
        } catch (IOException | RuntimeException e) { close(); throw failure(); }
        finally { timeout.cancel(false); }
    }

    private static IOException failure() { return new IOException("Sessao de voz encerrada"); }

    private List<InetAddress> resolve() throws IOException {
        CancellationSignal signal = new CancellationSignal();
        synchronized (lifecycle) { if (closed) throw failure(); dnsCancellation = signal; }
        try {
            DnsResolver.getInstance().query(null, HOST, DnsResolver.FLAG_EMPTY, Runnable::run, signal,
                    new DnsResolver.Callback<List<InetAddress>>() {
                        @Override public void onAnswer(List<InetAddress> answer, int rcode) {
                            if (rcode == 0 && !answer.isEmpty()) addresses.complete(answer);
                            else addresses.completeExceptionally(failure());
                        }
                        @Override public void onError(DnsResolver.DnsException error) { addresses.completeExceptionally(failure()); }
                    });
            List<InetAddress> resolved = addresses.get(10, TimeUnit.SECONDS);
            for (InetAddress address : resolved) HostPolicy.requirePublic(address);
            return resolved;
        } catch (InterruptedException e) { Thread.currentThread().interrupt(); throw failure(); }
        catch (ExecutionException | TimeoutException e) { throw failure(); }
        finally { signal.cancel(); }
    }

    Socket connectAddresses(List<InetAddress> destinations, long deadline) throws IOException {
        for (InetAddress destination : destinations) {
            long left = TimeUnit.NANOSECONDS.toMillis(deadline - System.nanoTime());
            if (closed || left <= 0) throw failure();
            Socket candidate = tcpFactory.create();
            synchronized (lifecycle) {
                if (closed) { candidate.close(); throw failure(); }
                tcp = candidate;
            }
            try {
                candidate.connect(new InetSocketAddress(destination, 443), (int) Math.min(3000, left));
                if (closed) throw failure();
                return candidate;
            } catch (IOException e) { candidate.close(); }
        }
        throw failure();
    }
    private static int byteFrom(InputStream in) throws IOException {
        int value = in.read(); if (value < 0) throw new EOFException(); return value;
    }
    private static void readFully(InputStream in, byte[] buffer, int offset, int length) throws IOException {
        while (length != 0) {
            int n = in.read(buffer, offset, length); if (n < 0) throw new EOFException();
            offset += n; length -= n;
        }
    }
    private static String readHeaders(InputStream in) throws IOException {
        ByteArrayOutputStream header = new ByteArrayOutputStream();
        int ending = 0;
        while (header.size() < 16384) {
            int b = byteFrom(in);
            if (b > 127 || b == 0) throw failure();
            header.write(b);
            ending = (ending << 8) | b;
            if (ending == 0x0d0a0d0a) return new String(header.toByteArray(), StandardCharsets.US_ASCII);
        }
        throw failure();
    }
    static void verifyUpgrade(String header, String challenge) throws IOException {
        String[] lines = header.split("\r\n");
        if (lines.length < 2 || !lines[0].matches("HTTP/1\\.1 101( .*)?")) throw failure();
        Map<String, String> fields = new HashMap<>();
        for (int i = 1; i < lines.length; i++) {
            int colon = lines[i].indexOf(':'); if (colon < 1) throw failure();
            String name = lines[i].substring(0, colon).toLowerCase(Locale.ROOT);
            if (fields.put(name, lines[i].substring(colon + 1).trim()) != null) throw failure();
        }
        try {
            String expected = Base64.getEncoder().encodeToString(MessageDigest.getInstance("SHA-1")
                    .digest((challenge + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").getBytes(StandardCharsets.US_ASCII)));
            String connection = fields.getOrDefault("connection", "").toLowerCase(Locale.ROOT);
            boolean upgrade = false;
            for (String token : connection.split(",")) if (token.trim().equals("upgrade")) upgrade = true;
            if (!expected.equals(fields.get("sec-websocket-accept")) || !upgrade
                    || !"websocket".equalsIgnoreCase(fields.get("upgrade"))
                    || fields.containsKey("sec-websocket-extensions") || fields.containsKey("sec-websocket-protocol")) throw failure();
        } catch (NoSuchAlgorithmException e) { throw failure(); }
    }

    private boolean readMessages(InputStream in, VoiceDuplex.Listener listener) throws IOException {
        return readMessages(in, listener, this::writeFrame);
    }
    @FunctionalInterface interface ControlWriter { void write(int opcode, byte[] payload) throws IOException; }
    static boolean readMessages(InputStream in, VoiceDuplex.Listener listener, ControlWriter control) throws IOException {
        byte[] message = new byte[VoicePacket.MAX_PAYLOAD];
        int used = 0, messageType = 0;
        while (true) {
            int first = byteFrom(in), second = byteFrom(in), opcode = first & 15;
            boolean fin = (first & 128) != 0;
            if ((first & 112) != 0 || (second & 128) != 0) throw failure();
            long size = second & 127;
            if (size == 126) {
                size = (byteFrom(in) << 8) | byteFrom(in); if (size < 126) throw failure();
            } else if (size == 127) {
                size = 0;
                for (int i = 0; i < 8; i++) {
                    int b = byteFrom(in);
                    if (i == 0 && b >= 128) throw failure();
                    size = (size << 8) | b;
                }
                if (size < 65536) throw failure();
            }
            if (opcode >= 8) {
                if (!fin || size > 125 || (opcode != 8 && opcode != 9 && opcode != 10)) throw failure();
                byte[] payload = new byte[(int) size]; readFully(in, payload, 0, payload.length);
                if (opcode == 8) {
                    if (size == 1) throw failure();
                    if (size > 2) VoicePacket.utf8(java.util.Arrays.copyOfRange(payload, 2, payload.length));
                    return size >= 2 && (((payload[0] & 255) << 8) | (payload[1] & 255)) == 1000;
                }
                if (opcode == 9) control.write(10, payload);
                continue;
            }
            if ((opcode == 0 && messageType == 0) || (opcode != 0 && messageType != 0)
                    || (opcode != 0 && opcode != 1 && opcode != 2) || size > VoicePacket.MAX_PAYLOAD - used) throw failure();
            if (opcode != 0) messageType = opcode;
            readFully(in, message, used, (int) size); used += (int) size;
            if (fin) {
                listener.message(VoicePacket.utf8(java.util.Arrays.copyOf(message, used)));
                used = 0; messageType = 0;
            }
        }
    }

    @Override public void send(String json) throws IOException {
        byte[] bytes = json.getBytes(StandardCharsets.UTF_8);
        if (bytes.length > VoicePacket.MAX_PAYLOAD) throw failure();
        writeFrame(1, bytes);
    }
    private void writeFrame(int opcode, byte[] payload) throws IOException {
        // No unbounded outbound WS queue. Relay ACK follows this bounded synchronous write.
        synchronized (writer) {
            OutputStream out = output; if (closed || out == null) throw failure();
            ScheduledFuture<?> timeout = DEADLINES.schedule(this::close, 15, TimeUnit.SECONDS);
            try {
                out.write(128 | opcode);
                int length = payload.length;
                if (length < 126) out.write(128 | length);
                else if (length <= 65535) { out.write(128 | 126); out.write(length >>> 8); out.write(length); }
                else {
                    out.write(128 | 127);
                    for (int shift = 56; shift >= 0; shift -= 8) out.write((int) ((long) length >>> shift));
                }
                byte[] mask = new byte[4]; random.nextBytes(mask); out.write(mask);
                byte[] chunk = new byte[Math.min(4096, payload.length)];
                for (int offset = 0; offset < length; offset += chunk.length) {
                    int n = Math.min(chunk.length, length - offset);
                    for (int i = 0; i < n; i++) chunk[i] = (byte) (payload[offset + i] ^ mask[(offset + i) & 3]);
                    out.write(chunk, 0, n);
                }
                out.flush();
                if (closed) throw failure();
            } catch (IOException e) { close(); throw failure(); }
            finally { timeout.cancel(false); }
        }
    }
    @Override public void close() {
        Socket raw; SSLSocket secure; CancellationSignal dns;
        synchronized (lifecycle) {
            if (closed) return;
            closed = true; raw = tcp; secure = tls; dns = dnsCancellation; output = null;
        }
        addresses.completeExceptionally(failure());
        if (dns != null) dns.cancel();
        if (raw != null) try { raw.close(); } catch (IOException ignored) { }
        if (secure != null) try { secure.close(); } catch (IOException ignored) { }
    }
}
