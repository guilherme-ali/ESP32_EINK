package br.com.ideiarec.companion.protocol;

import static org.junit.Assert.*;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.Set;
import org.junit.Test;

public class CodecPolicyWindowTest {
    @Test public void codecLittleEndianAndOwnership() {
        byte[] data = new byte[]{7, 9};
        Frame frame = new Frame(Frame.DATA, 3, 0x1234, 0xabcd, data);
        data[0] = 0;
        byte[] encoded = frame.encode();
        assertArrayEquals(new byte[]{1, 3, 3, 0x34, 0x12, (byte) 0xcd, (byte) 0xab, 7, 9}, encoded);
        Frame decoded = Frame.decode(encoded);
        assertEquals(0x1234, decoded.epoch); assertEquals(0xabcd, decoded.seq);
        encoded[7] = 0; byte[] payload = decoded.payload(); payload[0] = 1;
        assertArrayEquals(new byte[]{7, 9}, decoded.payload());
    }

    @Test public void rejectsMalformedEnvelopeAndControlFrames() {
        assertThrows(IllegalArgumentException.class, () -> Frame.decode(new byte[6]));
        assertThrows(IllegalArgumentException.class, () -> Frame.decode(new byte[181]));
        assertThrows(IllegalArgumentException.class, () -> Frame.decode(new byte[]{2, 3, 0, 0, 0, 0, 0}));
        assertThrows(IllegalArgumentException.class, () -> new Frame(3, 4, 0, 0, new byte[]{1}));
        assertThrows(IllegalArgumentException.class, () -> new Frame(4, 0, 0, 0, new byte[]{1}));
        assertThrows(IllegalArgumentException.class, () -> new Frame(2, 0, 0, 1, new byte[0]));
        assertThrows(IllegalArgumentException.class, () -> new Frame(1, 0, 0, 1, new byte[]{1, 2, 3}));
        assertThrows(IllegalArgumentException.class, () -> new Frame(3, 0, 0, 0, new byte[0]));
    }

    @Test public void exactHostNoUrlWildcardIpOrSuffixBypass() {
        HostPolicy policy = new HostPolicy(Set.of("api.openai.com", "EXAMPLE.COM"));
        assertEquals("example.com", policy.validate("EXAMPLE.COM", 443));
        for (String host : new String[]{"127.0.0.1", "127.1", "0x7f.0.0.1", "[::1]", "::1", "https://api.openai.com",
                "api.openai.com.", "api.openai.com.evil.test", "x.api.openai.com", "api.openai.com:443",
                "*.example.com", "a..com", "-x.example.com", " example.com", "exâmple.com"}) {
            assertThrows(host, IllegalArgumentException.class, () -> policy.validate(host, 443));
        }
        assertThrows(IllegalArgumentException.class, () -> policy.validate("api.openai.com", 80));
        assertThrows(IllegalArgumentException.class, () -> HostPolicy.utf8(new byte[]{(byte) 0xc3, 0x28}));
        assertEquals("example.com", HostPolicy.utf8("example.com".getBytes(StandardCharsets.UTF_8)));
    }

    @Test public void dnsCannotTargetPrivateOrLocalAddresses() throws Exception {
        for (String address : new String[]{"127.0.0.1", "10.0.0.1", "172.16.0.1", "192.168.1.1", "169.254.1.1",
                "100.64.0.1", "224.0.0.1", "0.0.0.0", "::1", "fc00::1", "fe80::1", "2001:db8::1"}) {
            InetAddress ip = InetAddress.getByName(address);
            assertThrows(address, java.net.UnknownHostException.class, () -> HostPolicy.requirePublic(ip));
        }
        HostPolicy.requirePublic(InetAddress.getByName("8.8.8.8"));
        HostPolicy.requirePublic(InetAddress.getByName("2606:4700:4700::1111"));
    }

    @Test public void windowRequiresIndividualHeadAckAndWraps16Bits() {
        SendWindow window = new SendWindow();
        for (int i = 0; i < 8; i++) assertEquals(i, window.reserve());
        assertTrue(window.full()); assertThrows(IllegalStateException.class, window::reserve);
        assertThrows(IllegalArgumentException.class, () -> window.ack(1));
        assertTrue(window.full());
        for (int i = 0; i < 8; i++) window.ack(i);
        assertThrows(IllegalArgumentException.class, () -> window.ack(7));
        for (int i = 8; i < 65536 + 8; i++) { int s = window.reserve(); assertEquals(i & 65535, s); window.ack(s); }
        assertTrue(window.empty());
    }
}
