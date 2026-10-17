package br.com.ideiarec.companion.protocol;

import java.util.ArrayDeque;

/** ACK individual deve corresponder exatamente ao DATA mais antigo. Thread-confined pelo engine. */
public final class SendWindow {
    public static final int CAPACITY = 8;
    private final ArrayDeque<Integer> pending = new ArrayDeque<>();
    private int next;
    public boolean full() { return pending.size() == CAPACITY; }
    public boolean empty() { return pending.isEmpty(); }
    public int reserve() {
        if (full()) throw new IllegalStateException("Janela cheia");
        int seq = next; next = (next + 1) & 65535; pending.add(seq); return seq;
    }
    public void ack(int seq) {
        if (pending.isEmpty() || pending.peek() != seq) throw new IllegalArgumentException("ACK fora de ordem");
        pending.remove();
    }
}
