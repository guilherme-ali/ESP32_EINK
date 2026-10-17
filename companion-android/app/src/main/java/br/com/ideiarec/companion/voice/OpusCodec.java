package br.com.ideiarec.companion.voice;

import java.io.IOException;

/** Official libopus 1.5.2, mono AUDIO, 20ms. No MediaCodec rate conversion or PLC. */
public final class OpusCodec implements VoiceDuplex.Codec {
    private long handle;
    public OpusCodec(int bitrate) throws IOException {
        if (bitrate != 64000 && bitrate != 48000 && bitrate != 32000) throw new IOException("Perfil Opus indisponivel");
        try {
            System.loadLibrary("voice_opus");
            handle = create(bitrate);
        } catch (LinkageError e) { throw new IOException("Opus indisponivel"); }
        if (handle == 0) throw new IOException("Opus indisponivel");
    }
    private void ready() throws IOException { if (handle == 0) throw new IOException("Codec fechado"); }
    @Override public synchronized int lookahead() throws IOException { ready(); return lookahead(handle); }
    @Override public synchronized byte[] encode(short[] pcm) throws IOException {
        ready(); if (pcm.length != 480) throw new IOException("Frame Opus invalido");
        byte[] result = encode(handle, pcm);
        if (result == null) throw new IOException("Falha Opus");
        return result;
    }
    @Override public synchronized short[] decode(byte[] packet) throws IOException {
        ready();
        if (packet.length == 0 || packet.length > 1276) throw new IOException("Packet Opus invalido");
        short[] result = decode(handle, packet);
        if (result == null || result.length != 320) throw new IOException("Frame Opus invalido");
        return result;
    }
    @Override public synchronized void resetEncoder() throws IOException { ready(); if (!reset(handle, true)) throw new IOException("Falha Opus"); }
    @Override public synchronized void resetDecoder() throws IOException { ready(); if (!reset(handle, false)) throw new IOException("Falha Opus"); }
    @Override public synchronized void close() { if (handle != 0) { destroy(handle); handle = 0; } }
    private static native long create(int bitrate);
    private static native int lookahead(long handle);
    private static native byte[] encode(long handle, short[] pcm);
    private static native short[] decode(long handle, byte[] packet);
    private static native boolean reset(long handle, boolean encoder);
    private static native void destroy(long handle);
}
