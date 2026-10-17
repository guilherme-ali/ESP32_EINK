package br.com.ideiarec.companion.voice;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** Records carried inside the existing relay DATA byte stream, not new relay frames. */
public final class VoicePacket {
    public static final int HEADER = 12, MAX_PAYLOAD = 262144, MAX_PCM = 192 * 1024;
    public static final int HELLO = 1, JSON = 2, OPUS = 3, PCM = 4, END_INPUT = 5, ERROR = 6;
    public static final int RESET_DECODER = 1;
    public final int type, flags;
    public final long position;
    public final byte[] payload;

    public VoicePacket(int type, int flags, long position, byte[] payload) throws IOException {
        validate(type, flags, position, payload.length);
        this.type = type; this.flags = flags; this.position = position; this.payload = payload.clone();
    }

    private static void validate(int type, int flags, long position, long length) throws IOException {
        if (type < HELLO || type > ERROR || flags < 0 || (flags != 0 && (type != OPUS || flags != RESET_DECODER))
                || position < 0 || position > 0xffffffffL || length < 0 || length > MAX_PAYLOAD
                || (type == PCM && (length == 0 || length > MAX_PCM || (length & 1) != 0))
                || (type == OPUS && (length < 5 || length > 1280))
                || (type == END_INPUT && length != 0)) throw new IOException("Registro de voz invalido");
    }

    public byte[] encode() {
        return ByteBuffer.allocate(HEADER + payload.length).order(ByteOrder.LITTLE_ENDIAN)
                .put((byte) 0x56).put((byte) 0x31).put((byte) type).put((byte) flags)
                .putInt(payload.length).putInt((int) position).put(payload).array();
    }

    static String utf8(byte[] value) throws IOException {
        try {
            return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(value)).toString();
        } catch (CharacterCodingException e) { throw new IOException("UTF-8 invalido"); }
    }

    @FunctionalInterface public interface Receiver { void receive(VoicePacket packet) throws IOException; }

    public static final class Decoder {
        private final byte[] header = new byte[HEADER];
        private byte[] body;
        private int headerUsed, bodyUsed, type, flags;
        private long position;
        public void accept(byte[] bytes, int offset, int length, Receiver receiver) throws IOException {
            java.util.Objects.checkFromIndexSize(offset, length, bytes.length);
            while (length > 0) {
                if (headerUsed < HEADER) {
                    int count = Math.min(length, HEADER - headerUsed);
                    System.arraycopy(bytes, offset, header, headerUsed, count);
                    headerUsed += count; offset += count; length -= count;
                    if (headerUsed != HEADER) continue;
                    ByteBuffer h = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
                    if (h.get() != 0x56 || h.get() != 0x31) throw new IOException("Magic de voz invalido");
                    type = h.get() & 255; flags = h.get() & 255;
                    long size = Integer.toUnsignedLong(h.getInt()); position = Integer.toUnsignedLong(h.getInt());
                    validate(type, flags, position, size);
                    body = new byte[(int) size];
                }
                int count = Math.min(length, body.length - bodyUsed);
                System.arraycopy(bytes, offset, body, bodyUsed, count);
                bodyUsed += count; offset += count; length -= count;
                if (bodyUsed == body.length) {
                    VoicePacket packet = new VoicePacket(type, flags, position, body);
                    headerUsed = 0; bodyUsed = 0; Arrays.fill(body, (byte) 0); body = null;
                    receiver.receive(packet);
                }
            }
        }
        public void clear() {
            Arrays.fill(header, (byte) 0);
            if (body != null) Arrays.fill(body, (byte) 0);
            body = null; headerUsed = 0; bodyUsed = 0;
        }
    }
}
