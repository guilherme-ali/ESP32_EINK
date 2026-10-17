package br.com.ideiarec.companion.protocol;

import java.net.InetAddress;
import java.net.UnknownHostException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Collection;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

/** Somente nomes DNS ASCII exatos e destinos públicos, porta 443. */
public final class HostPolicy {
    public static final Set<String> DEFAULTS = Set.of(
            "generativelanguage.googleapis.com", "www.googleapis.com", "oauth2.googleapis.com",
            "accounts.google.com", "script.google.com", "script.googleusercontent.com",
            "api.github.com", "api.groq.com", "api.openai.com");
    private final Set<String> allowed;

    public HostPolicy(Collection<String> hosts) {
        Set<String> copy = new HashSet<>();
        for (String h : hosts) copy.add(normalize(h));
        allowed = Set.copyOf(copy);
    }

    public static String normalize(String host) {
        if (host == null || host.length() > 253 || !host.equals(host.trim())) bad();
        String h = host.toLowerCase(Locale.ROOT);
        if (!h.matches("[a-z0-9-]+(?:\\.[a-z0-9-]+)+") || h.matches("[0-9.]+")) bad();
        for (String label : h.split("\\.")) {
            if (label.length() > 63 || label.startsWith("-") || label.endsWith("-")) bad();
        }
        // Bloqueia também formas abreviadas/alternativas de literais IPv4.
        if (!h.substring(h.lastIndexOf('.') + 1).matches("[a-z][a-z0-9-]*")) bad();
        return h;
    }

    public String validate(String host, int port) {
        String h = normalize(host);
        if (port != 443 || !allowed.contains(h)) bad();
        return h;
    }

    public static String utf8(byte[] data) {
        try {
            return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(data)).toString();
        } catch (CharacterCodingException e) { throw new IllegalArgumentException("UTF-8 inválido"); }
    }

    public static void requirePublic(InetAddress address) throws UnknownHostException {
        byte[] a = address.getAddress();
        boolean blocked = address.isAnyLocalAddress() || address.isLoopbackAddress()
                || address.isLinkLocalAddress() || address.isSiteLocalAddress() || address.isMulticastAddress();
        if (a.length == 4) {
            int x = a[0] & 255, y = a[1] & 255;
            blocked |= x == 0 || x >= 224 || (x == 100 && y >= 64 && y <= 127)
                    || (x == 192 && (y == 0 || y == 2 || y == 168))
                    || (x == 198 && (y == 18 || y == 19 || y == 51))
                    || (x == 203 && y == 0);
        } else {
            // IPv6 global unicast 2000::/3; exclui documentação 2001:db8::/32.
            blocked |= (a[0] & 0xe0) != 0x20
                    || (a[0] == 0x20 && a[1] == 1 && (a[2] & 255) == 0x0d && (a[3] & 255) == 0xb8);
        }
        if (blocked) throw new UnknownHostException("Destino não público");
    }

    private static void bad() { throw new IllegalArgumentException("Host/porta não permitido"); }
}
