package br.com.ideiarec.companion;

import br.com.ideiarec.companion.protocol.HostPolicy;
import br.com.ideiarec.companion.protocol.RelayEngine;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.Collections;
import javax.net.ssl.SNIHostName;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

/** Trust store padrão Android. O nome original é preservado no TLS/SNI/verificação HTTPS. */
final class TlsSocketFactory implements RelayEngine.SocketFactory {
    @Override public RelayEngine.Duplex create() { return new Connection(); }

    private static final class Connection implements RelayEngine.Duplex {
        private Socket tcp;
        private SSLSocket tls;
        private boolean closed;

        @Override public void connect(String hostname, int port) throws IOException {
            InetAddress[] addresses = InetAddress.getAllByName(hostname);
            if (addresses.length == 0) throw new IOException("DNS vazio");
            // Valida todas e fixa o IP escolhido: não há uma segunda resolução no connect TLS.
            for (InetAddress a : addresses) HostPolicy.requirePublic(a);
            Socket raw = new Socket();
            synchronized (this) {
                if (closed) { raw.close(); throw new IOException("Cancelado"); }
                tcp = raw;
            }
            raw.connect(new InetSocketAddress(addresses[0], port), 10_000);
            raw.setSoTimeout(15_000);
            SSLSocket secure = (SSLSocket) ((SSLSocketFactory) SSLSocketFactory.getDefault())
                    .createSocket(raw, hostname, port, true);
            synchronized (this) {
                if (closed) { secure.close(); throw new IOException("Cancelado"); }
                tls = secure;
            }
            SSLParameters parameters = secure.getSSLParameters();
            parameters.setEndpointIdentificationAlgorithm("HTTPS");
            parameters.setServerNames(Collections.singletonList(new SNIHostName(hostname)));
            secure.setSSLParameters(parameters);
            secure.setSoTimeout(15_000);
            secure.startHandshake();
            secure.setSoTimeout(90_000);
        }

        @Override public synchronized InputStream input() throws IOException {
            if (closed || tls == null) throw new IOException("Fechado");
            return tls.getInputStream();
        }
        @Override public synchronized OutputStream output() throws IOException {
            if (closed || tls == null) throw new IOException("Fechado");
            return tls.getOutputStream();
        }
        @Override public synchronized void close() {
            closed = true;
            // Fecha TCP primeiro para interromper inclusive write/handshake, sem esperar close_notify TLS.
            if (tcp != null) try { tcp.close(); } catch (IOException ignored) { }
            if (tls != null) try { tls.close(); } catch (IOException ignored) { }
        }
    }
}
