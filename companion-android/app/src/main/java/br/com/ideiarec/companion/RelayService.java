package br.com.ideiarec.companion;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.SystemClock;
import br.com.ideiarec.companion.protocol.HostPolicy;
import br.com.ideiarec.companion.protocol.RelayEngine;
import br.com.ideiarec.companion.voice.VoiceDuplex;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

public final class RelayService extends Service {
    static volatile boolean linkConnected;
    static volatile boolean running;
    private static volatile RelayService owner;
    private static final String CHANNEL = "ideiarec-relay";
    private static final int NOTIFICATION = 1;
    private Config config;
    private HandlerThread thread;
    private Handler handler;
    private GattTransport transport;
    private RelayEngine engine;
    private ThreadPoolExecutor io;
    private final AtomicBoolean stopping = new AtomicBoolean();
    private boolean started;
    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (stopping.get()) return;
            if (engine != null) engine.tick();
            handler.postDelayed(this, 1000);
        }
    };

    @Override public void onCreate() {
        super.onCreate(); owner = this; linkConnected = false; config = new Config(this);
        NotificationManager notifications = getSystemService(NotificationManager.class);
        notifications.createNotificationChannel(new NotificationChannel(CHANNEL, "Ponte TLS IdeiaRec", NotificationManager.IMPORTANCE_LOW));
        thread = new HandlerThread("ideiarec-gatt"); thread.start(); handler = new Handler(thread.getLooper());
        io = new ThreadPoolExecutor(12, 12, 0, TimeUnit.MILLISECONDS, new ArrayBlockingQueue<>(32), runnable -> {
            Thread worker = new Thread(runnable, "ideiarec-tls"); worker.setDaemon(true); return worker;
        }, new ThreadPoolExecutor.AbortPolicy());
    }

    private Notification notification() {
        Intent open = new Intent(this, MainActivity.class);
        PendingIntent content = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        PendingIntent stop = PendingIntent.getService(this, 1, new Intent(this, RelayService.class).setAction("STOP"),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        return new Notification.Builder(this, CHANNEL).setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setContentTitle("IdeiaRec — ponte TLS ativa").setContentText("Conectando ou conectado ao ESP32")
                .setContentIntent(content).setOngoing(true)
                .addAction(new Notification.Action.Builder(null, "Parar", stop).build()).build();
    }

    @Override public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && "STOP".equals(intent.getAction())) {
            config.enabled(false);
            try { Presence.observe(this, false); } catch (RuntimeException ignored) { }
            finish("Parado pelo usuário"); return START_NOT_STICKY;
        }
        if (stopping.get()) return START_NOT_STICKY;
        if (!config.enabled() || config.mac().isEmpty()
                || checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED
                || checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED) {
            finish("Associe e conceda permissões Bluetooth"); return START_NOT_STICKY;
        }
        try {
            startForeground(NOTIFICATION, notification(), ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            running = true;
        }
        catch (RuntimeException e) { finish("Foreground recusado; abra o app"); return START_NOT_STICKY; }
        if (!started) { started = true; handler.post(this::connect); }
        return START_NOT_STICKY;
    }

    @SuppressLint("MissingPermission")
    private void connect() {
        if (stopping.get()) return;
        try {
            BluetoothManager manager = getSystemService(BluetoothManager.class);
            BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
            if (adapter == null || !adapter.isEnabled()) { finish("Ative o Bluetooth"); return; }
            config.status("Conectando; aguardando MTU, bond e criptografia");
            transport = new GattTransport(this, handler, adapter.getRemoteDevice(config.mac()), new GattTransport.Listener() {
                @Override public void connected() { if (owner == RelayService.this) linkConnected = true; }
                @Override public void ready() {
                    if (stopping.get()) return;
                    RelayEngine.SocketFactory sockets = new RelayEngine.SocketFactory() {
                        @Override public RelayEngine.Duplex create() { return new TlsSocketFactory().create(); }
                        @Override public boolean supportsVoice() { return true; }
                        @Override public RelayEngine.Duplex createVoice() { return new VoiceDuplex(); }
                    };
                    engine = new RelayEngine(new HostPolicy(config.hosts()), sockets, transport, io, SystemClock::elapsedRealtime);
                    config.status("Pronto — GATT seguro; aguardando OPEN do ESP32; " + transport.linkMetricsText());
                    handler.post(tick);
                }
                @Override public void linkMetrics(String metrics) { config.status(metrics); }
                @Override public void frame(byte[] value) {
                    try { if (engine != null && !stopping.get()) engine.receive(value); }
                    catch (RuntimeException e) { finish("Protocolo inválido; sessão encerrada"); }
                }
                @Override public void failed(String status) { finish(status); }
            });
            transport.connect();
        } catch (RuntimeException e) { finish("Falha Bluetooth/configuração: " + e.getClass().getSimpleName()); }
    }

    private void finish(String status) {
        config.status(status);
        if (stopping.compareAndSet(false, true)) {
            handler.post(this::cleanup);
            stopSelf();
        }
    }

    private void cleanup() {
        handler.removeCallbacks(tick);
        if (engine != null) { engine.close(); engine = null; }
        if (transport != null) { transport.close(); transport = null; }
        if (owner == this) { linkConnected = false; running = false; }
        if (config != null && config.enabled()) {
            try { Presence.observe(this, true); } catch (RuntimeException ignored) { }
        }
        io.shutdownNow(); thread.quitSafely();
    }

    @Override public void onDestroy() {
        stopping.set(true);
        handler.post(this::cleanup);
        stopForeground(STOP_FOREGROUND_REMOVE);
        super.onDestroy();
    }
    @Override public IBinder onBind(Intent intent) { return null; }
}
