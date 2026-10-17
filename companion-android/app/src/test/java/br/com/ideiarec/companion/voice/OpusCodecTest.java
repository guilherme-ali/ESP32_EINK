package br.com.ideiarec.companion.voice;

import static org.junit.Assert.*;
import java.io.IOException;
import org.junit.Test;

/** Real JNI + pinned official libopus; build_android.py supplies the host library. */
public class OpusCodecTest {
    @Test public void allExactProfilesHave24kEncoder16kDecoderAndCorrectLookahead() throws Exception {
        for (int bitrate : new int[]{64000, 48000, 32000}) {
            try (OpusCodec codec = new OpusCodec(bitrate)) {
                assertEquals(156, codec.lookahead());
                long energy = 0;
                for (int frame = 0; frame < 100; frame++) {
                    short[] pcm = new short[480];
                    for (int i = 0; i < pcm.length; i++) pcm[i] = (short) (10000 * Math.sin(2 * Math.PI * 440 * (frame * 480 + i) / 24000));
                    byte[] packet = codec.encode(pcm); assertTrue(packet.length > 0); assertTrue(packet.length <= 1276);
                    short[] decoded = codec.decode(packet); assertEquals(320, decoded.length);
                    for (short value : decoded) energy += (long) value * value;
                }
                assertTrue("Non-silent real libopus output", energy > 100000000);
            }
        }
    }
    @Test public void independentResetsReproduceEncoderAndDecoderWithoutCrossReset() throws Exception {
        try (OpusCodec codec = new OpusCodec(64000); OpusCodec reference = new OpusCodec(64000)) {
            short[] pcm = new short[480]; for (int i = 0; i < pcm.length; i++) pcm[i] = (short) (i * 17);
            byte[] first = codec.encode(pcm); byte[] refFirst = reference.encode(pcm); assertArrayEquals(refFirst, first);
            short[] decodedFirst = codec.decode(first); reference.decode(first);
            codec.resetEncoder(); assertArrayEquals(first, codec.encode(pcm));
            byte[] next = reference.encode(pcm);
            assertArrayEquals(reference.decode(next), codec.decode(next)); // encoder reset left decoder state intact.
            codec.resetDecoder(); assertArrayEquals(decodedFirst, codec.decode(first));
            assertArrayEquals(next, codec.encode(pcm)); // decoder reset left encoder state intact.
        }
    }
    @Test public void malformedFramesNoPlcUnsupportedProfileAndClosedCodecFail() throws Exception {
        assertThrows(IOException.class, () -> new OpusCodec(16000));
        OpusCodec codec = new OpusCodec(64000);
        assertThrows(IOException.class, () -> codec.encode(new short[320]));
        assertThrows(IOException.class, () -> codec.decode(new byte[0]));
        assertThrows(IOException.class, () -> codec.decode(new byte[]{(byte) 0x80})); // 2.5ms, not a 20ms frame.
        codec.close(); codec.close(); assertThrows(IOException.class, () -> codec.encode(new short[480]));
    }
}
