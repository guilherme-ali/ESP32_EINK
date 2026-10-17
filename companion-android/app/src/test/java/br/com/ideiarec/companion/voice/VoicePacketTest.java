package br.com.ideiarec.companion.voice;

import static org.junit.Assert.*;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.List;
import org.junit.Test;

public class VoicePacketTest {
    @Test public void littleEndianWireAndArbitraryFragmentationIncludingEmptyRecord() throws Exception {
        VoicePacket a = new VoicePacket(VoicePacket.PCM, 0, 0x87654321L, new byte[]{1, 2});
        assertArrayEquals(new byte[]{0x56, 0x31, 4, 0, 2, 0, 0, 0, 0x21, 0x43, 0x65, (byte) 0x87, 1, 2}, a.encode());
        ByteArrayOutputStream joined = new ByteArrayOutputStream(); joined.write(a.encode());
        joined.write(new VoicePacket(VoicePacket.END_INPUT, 0, 1, new byte[0]).encode());
        VoicePacket.Decoder decoder = new VoicePacket.Decoder(); List<VoicePacket> result = new ArrayList<>();
        for (byte b : joined.toByteArray()) decoder.accept(new byte[]{b}, 0, 1, result::add);
        assertEquals(2, result.size()); assertEquals(0x87654321L, result.get(0).position);
        assertArrayEquals(new byte[]{1, 2}, result.get(0).payload); assertEquals(VoicePacket.END_INPUT, result.get(1).type);
    }
    @Test public void rejectsOversizeUnsignedLengthBeforeBodyAllocation() {
        for (long length : new long[]{262145, 0xffffffffL}) {
            byte[] h = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN).put((byte) 0x56).put((byte) 0x31)
                    .put((byte) 2).put((byte) 0).putInt((int) length).putInt(0).array();
            assertThrows(IOException.class, () -> new VoicePacket.Decoder().accept(h, 0, h.length, p -> fail()));
        }
    }
    @Test public void validatesFlagsPcmAlignmentOpusAndEndInput() throws Exception {
        assertThrows(IOException.class, () -> new VoicePacket(4, 1, 0, new byte[2]));
        assertThrows(IOException.class, () -> new VoicePacket(3, 2, 0, new byte[5]));
        assertThrows(IOException.class, () -> new VoicePacket(4, 0, 0, new byte[3]));
        assertThrows(IOException.class, () -> new VoicePacket(4, 0, 0, new byte[VoicePacket.MAX_PCM + 2]));
        assertThrows(IOException.class, () -> new VoicePacket(3, 0, 0, new byte[4]));
        assertThrows(IOException.class, () -> new VoicePacket(5, 0, 0, new byte[1]));
        assertThrows(IOException.class, () -> new VoicePacket(2, 0, 0x100000000L, new byte[0]));
        new VoicePacket(3, 1, 0, new byte[1280]);
    }
    @Test public void rejectsInvalidMagicAndUtf8() {
        byte[] h = new byte[12];
        assertThrows(IOException.class, () -> new VoicePacket.Decoder().accept(h, 0, h.length, p -> fail()));
        assertThrows(IOException.class, () -> VoicePacket.utf8(new byte[]{(byte) 0xc0, (byte) 0x80}));
    }
}
