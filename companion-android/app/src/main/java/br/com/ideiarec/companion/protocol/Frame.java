package br.com.ideiarec.companion.protocol;

import java.util.Arrays;

/** Um valor GATT contém exatamente um frame; não há remontagem entre notificações. */
public final class Frame {
    public static final int OPEN = 1, OPENED = 2, DATA = 3, ACK = 4, CLOSE = 5, ERROR = 6;
    public static final int HEADER = 7, MAX_PAYLOAD = 173, MAX_FRAME = 180;
    public final int type, channel, epoch, seq;
    private final byte[] payload;

    public Frame(int type, int channel, int epoch, int seq, byte[] payload) {
        if (type < OPEN || type > ERROR || channel < 0 || channel >= 4
                || epoch < 0 || epoch > 65535 || seq < 0 || seq > 65535
                || payload == null || payload.length > MAX_PAYLOAD) {
            throw new IllegalArgumentException("Frame inválido");
        }
        if (((type == OPENED || type == ACK || type == CLOSE) && payload.length != 0)
                || (type == OPEN && (payload.length < 3 || seq != 0))
                || (type == OPENED && seq != 0) || (type == DATA && payload.length == 0)) {
            throw new IllegalArgumentException("Payload/seq inválido");
        }
        this.type = type; this.channel = channel; this.epoch = epoch; this.seq = seq;
        this.payload = payload.clone();
    }

    public byte[] payload() { return payload.clone(); }

    public byte[] encode() {
        byte[] out = new byte[HEADER + payload.length];
        out[0] = 1; out[1] = (byte) type; out[2] = (byte) channel;
        out[3] = (byte) epoch; out[4] = (byte) (epoch >>> 8);
        out[5] = (byte) seq; out[6] = (byte) (seq >>> 8);
        System.arraycopy(payload, 0, out, HEADER, payload.length);
        return out;
    }

    public static Frame decode(byte[] bytes) {
        if (bytes == null || bytes.length < HEADER || bytes.length > MAX_FRAME || bytes[0] != 1) {
            throw new IllegalArgumentException("Envelope inválido");
        }
        return new Frame(bytes[1] & 255, bytes[2] & 255,
                (bytes[3] & 255) | ((bytes[4] & 255) << 8),
                (bytes[5] & 255) | ((bytes[6] & 255) << 8),
                Arrays.copyOfRange(bytes, HEADER, bytes.length));
    }
}
