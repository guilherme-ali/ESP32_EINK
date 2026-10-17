package br.com.ideiarec.companion.protocol;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.BitSet;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executor;
import java.util.concurrent.RejectedExecutionException;
import java.util.function.BooleanSupplier;
import java.util.function.LongSupplier;

/** Núcleo sem Android. Lock protege apenas estado; nenhuma espera/I/O acontece sob o lock. */
public final class RelayEngine implements AutoCloseable {
    public interface Duplex extends AutoCloseable {
        void connect(String hostname, int port) throws IOException;
        InputStream input() throws IOException;
        OutputStream output() throws IOException;
        @Override void close();
    }
    public interface SocketFactory { Duplex create() throws IOException; }
    public interface BleWriter {
        /** Enfileira imediatamente. Verificar stillValid antes de iniciar a operação GATT. */
        CompletableFuture<Void> send(byte[] frame, BooleanSupplier stillValid);
    }

    private final HostPolicy policy;
    private final SocketFactory sockets;
    private final BleWriter ble;
    private final Executor io;
    private final LongSupplier clock;
    private final Channel[] channels = new Channel[4];
    private final BitSet[] usedEpochs = {new BitSet(65536), new BitSet(65536), new BitSet(65536), new BitSet(65536)};
    private boolean shutdown;
    private static final long OPEN_TIMEOUT = 20_000, IO_TIMEOUT = 20_000, IDLE_TIMEOUT = 90_000;

    private static final class Channel {
        final int id, epoch;
        final String host;
        final SendWindow tx = new SendWindow();
        final ArrayDeque<Long> sentAt = new ArrayDeque<>();
        final ArrayDeque<Frame> rx = new ArrayDeque<>();
        Duplex socket;
        boolean closed, ready, writer, eof;
        int expected;
        long openedAt, activityAt, writeAt;
        Channel(int id, int epoch, String host, long now) {
            this.id = id; this.epoch = epoch; this.host = host; openedAt = now; activityAt = now;
        }
    }

    public RelayEngine(HostPolicy policy, SocketFactory sockets, BleWriter ble, Executor io, LongSupplier clock) {
        this.policy = policy; this.sockets = sockets; this.ble = ble; this.io = io; this.clock = clock;
    }

    public synchronized void receive(byte[] encoded) {
        if (shutdown) return;
        Frame f = Frame.decode(encoded); // Envelope inválido: transportador deve abortar a conexão inteira.
        Channel c = channels[f.channel];
        if (f.type == Frame.OPEN) {
            if (usedEpochs[f.channel].get(f.epoch)) return;
            usedEpochs[f.channel].set(f.epoch);
            if (c != null) closeChannel(c, false, false);
            String host;
            try {
                byte[] p = f.payload();
                int port = (p[0] & 255) | ((p[1] & 255) << 8);
                host = policy.validate(HostPolicy.utf8(Arrays.copyOfRange(p, 2, p.length)), port);
            } catch (IllegalArgumentException e) {
                Channel denied = new Channel(f.channel, f.epoch, "", clock.getAsLong());
                channels[f.channel] = denied;
                closeChannel(denied, true, true);
                return;
            }
            Channel fresh = new Channel(f.channel, f.epoch, host, clock.getAsLong());
            channels[f.channel] = fresh;
            execute(fresh, () -> open(fresh));
            return;
        }
        if (c == null || c.closed || c.epoch != f.epoch) return;
        switch (f.type) {
            case Frame.DATA:
                if (!c.ready || f.seq != c.expected || c.rx.size() >= SendWindow.CAPACITY) {
                    closeChannel(c, true, true); return;
                }
                c.expected = (c.expected + 1) & 65535;
                c.rx.add(f); c.activityAt = clock.getAsLong();
                if (!c.writer) {
                    c.writer = true; c.writeAt = clock.getAsLong();
                    execute(c, () -> writeLoop(c));
                }
                break;
            case Frame.ACK:
                try { c.tx.ack(f.seq); c.sentAt.remove(); }
                catch (IllegalArgumentException e) { closeChannel(c, true, true); return; }
                c.activityAt = clock.getAsLong(); notifyAll();
                if (c.eof && c.tx.empty()) closeChannel(c, true, false);
                break;
            case Frame.CLOSE:
            case Frame.ERROR:
                closeChannel(c, true, false); break;
            default:
                closeChannel(c, true, true); break;
        }
    }

    private void open(Channel c) {
        Duplex s = null;
        try {
            s = sockets.create(); // Deve ser não bloqueante; registro anterior a DNS/connect/handshake.
            synchronized (this) {
                if (c.closed) { s.close(); return; }
                c.socket = s;
            }
            s.connect(c.host, 443);
            synchronized (this) {
                if (c.closed) return;
                // O ESP pode notificar DATA antes de Android despachar o callback do write OPENED.
                // TLS já está pronto; aceita RX, mas inicia o reader só após concluir esse write.
                c.ready = true; c.activityAt = clock.getAsLong();
                emit(c, Frame.OPENED, 0, new byte[0], false).whenComplete((unused, error) -> {
                    synchronized (RelayEngine.this) {
                        if (error == null && !c.closed) {
                            execute(c, () -> readLoop(c));
                        }
                    }
                });
            }
        } catch (IOException | RuntimeException e) { fail(c); }
        finally {
            synchronized (this) { if (c.closed && s != null) s.close(); }
        }
    }

    private void readLoop(Channel c) {
        try {
            InputStream in = c.socket.input();
            byte[] buffer = new byte[Frame.MAX_PAYLOAD];
            while (true) {
                synchronized (this) {
                    while (!c.closed && c.tx.full()) wait(IO_TIMEOUT);
                    if (c.closed) return;
                }
                int n = in.read(buffer);
                if (n == -1) {
                    synchronized (this) {
                        c.eof = true;
                        // Não invalida DATA ainda na fila BLE: drena os ACKs antes do CLOSE normal.
                        if (c.tx.empty()) closeChannel(c, true, false);
                    }
                    return;
                }
                if (n == 0) continue;
                synchronized (this) {
                    if (c.closed) return;
                    int seq = c.tx.reserve();
                    c.sentAt.add(clock.getAsLong()); c.activityAt = clock.getAsLong();
                    emit(c, Frame.DATA, seq, Arrays.copyOf(buffer, n), false);
                }
            }
        } catch (IOException | InterruptedException | RuntimeException e) {
            if (e instanceof InterruptedException) Thread.currentThread().interrupt();
            fail(c);
        }
    }

    private void writeLoop(Channel c) {
        try {
            OutputStream out = c.socket.output();
            while (true) {
                Frame f;
                synchronized (this) {
                    if (c.closed) return;
                    f = c.rx.peek();
                    if (f == null) { c.writer = false; return; }
                    c.writeAt = clock.getAsLong();
                }
                out.write(f.payload()); // ACK só após o consumo efetivo pelo OutputStream TLS.
                synchronized (this) {
                    if (c.closed) return;
                    c.rx.remove(); c.activityAt = clock.getAsLong();
                    emit(c, Frame.ACK, f.seq, new byte[0], false);
                }
            }
        } catch (IOException | RuntimeException e) { fail(c); }
    }

    private CompletableFuture<Void> emit(Channel c, int type, int seq, byte[] p, boolean terminal) {
        CompletableFuture<Void> future;
        try {
            future = ble.send(new Frame(type, c.id, c.epoch, seq, p).encode(), () -> {
                synchronized (RelayEngine.this) {
                    return !shutdown && channels[c.id] == c && (terminal || !c.closed);
                }
            });
        } catch (RuntimeException e) { future = CompletableFuture.failedFuture(e); }
        if (!terminal) future.whenComplete((unused, e) -> { if (e != null) fail(c); });
        return future;
    }

    private void execute(Channel c, Runnable task) {
        try { io.execute(task); }
        catch (RejectedExecutionException e) { closeChannel(c, true, true); }
    }

    private synchronized void fail(Channel c) { closeChannel(c, true, true); }

    private void closeChannel(Channel c, boolean notifyPeer, boolean error) {
        if (c.closed) return;
        c.closed = true; c.rx.clear(); notifyAll();
        if (c.socket != null) c.socket.close(); // close não aguarda workers; desbloqueia I/O em andamento.
        if (notifyPeer && !shutdown) {
            emit(c, error ? Frame.ERROR : Frame.CLOSE, 0,
                    error ? new byte[]{'C', 'a', 'n', 'a', 'l', ' ', 'e', 'n', 'c', 'e', 'r', 'r', 'a', 'd', 'o'} : new byte[0], true);
        }
    }

    /** Chamado por scheduler externo; nenhum timer por pacote. */
    public synchronized void tick() {
        long now = clock.getAsLong();
        for (Channel c : channels) {
            if (c == null || c.closed) continue;
            if ((!c.ready && now - c.openedAt >= OPEN_TIMEOUT)
                    || (!c.sentAt.isEmpty() && now - c.sentAt.peek() >= IO_TIMEOUT)
                    || (c.writer && now - c.writeAt >= IO_TIMEOUT)
                    || now - c.activityAt >= IDLE_TIMEOUT) closeChannel(c, true, true);
        }
    }

    @Override public synchronized void close() {
        if (shutdown) return;
        shutdown = true;
        for (Channel c : channels) if (c != null) closeChannel(c, false, false);
    }
}
