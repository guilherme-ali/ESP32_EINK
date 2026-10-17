package br.com.ideiarec.companion;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothStatusCodes;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.os.Build;
import android.os.Handler;
import br.com.ideiarec.companion.protocol.Frame;
import br.com.ideiarec.companion.protocol.RelayEngine;
import java.util.ArrayDeque;
import java.util.UUID;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.function.BooleanSupplier;

/** Uma conexão, um handler, uma operação em voo. Timeout invalida o GATT inteiro. */
@SuppressLint("MissingPermission") // RelayService valida runtime antes de criar; revogação aborta a sessão.
@SuppressWarnings("deprecation")
final class GattTransport implements RelayEngine.BleWriter, AutoCloseable {
    static final UUID SERVICE = UUID.fromString("7fd00001-6f6e-4d69-9b18-8e8c49b5c100");
    private static final UUID RX = UUID.fromString("7fd00002-6f6e-4d69-9b18-8e8c49b5c100");
    private static final UUID TX = UUID.fromString("7fd00003-6f6e-4d69-9b18-8e8c49b5c100");
    private static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    interface Listener {
        void connected();
        void ready();
        void frame(byte[] value);
        void failed(String status);
    }
    private enum Kind { MTU, DISCOVER, SUBSCRIBE, WRITE }
    private interface Start { boolean run(); }
    private static final class Operation {
        final Kind kind;
        final Start start;
        final Runnable done;
        final BooleanSupplier valid;
        final CompletableFuture<Void> result;
        Operation(Kind kind, Start start, Runnable done, BooleanSupplier valid, CompletableFuture<Void> result) {
            this.kind = kind; this.start = start; this.done = done; this.valid = valid; this.result = result;
        }
    }

    private final Context context;
    private final Handler handler;
    private final BluetoothDevice device;
    private final Listener listener;
    private final ArrayDeque<Operation> queue = new ArrayDeque<>();
    private final ArrayDeque<byte[]> earlyNotifications = new ArrayDeque<>();
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic rx, tx;
    private BluetoothGattDescriptor cccd;
    private Operation active;
    private boolean closed, registered, discovered, subscribing, encrypted, ready;
    private final Runnable operationTimeout = () -> abort("Timeout GATT; sessão descartada");
    private final Runnable startupTimeout = () -> abort("Timeout de conexão, bond ou criptografia");

    GattTransport(Context context, Handler handler, BluetoothDevice device, Listener listener) {
        this.context = context; this.handler = handler; this.device = device; this.listener = listener;
    }

    void connect() {
        try {
            IntentFilter filter = new IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED);
            if (Build.VERSION.SDK_INT >= 36) filter.addAction(BluetoothDevice.ACTION_ENCRYPTION_CHANGE);
            if (Build.VERSION.SDK_INT >= 33) context.registerReceiver(security, filter,
                    android.Manifest.permission.BLUETOOTH_CONNECT, handler, Context.RECEIVER_EXPORTED);
            else context.registerReceiver(security, new IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED),
                    android.Manifest.permission.BLUETOOTH_CONNECT, handler);
            registered = true;
            handler.postDelayed(startupTimeout, 90_000);
            gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE,
                    BluetoothDevice.PHY_LE_1M_MASK, handler);
            if (gatt == null) abort("Falha ao abrir GATT");
        } catch (RuntimeException e) { abort("Bluetooth indisponível ou permissão revogada"); }
    }

    private final BroadcastReceiver security = new BroadcastReceiver() {
        @Override public void onReceive(Context ctx, Intent intent) {
            BluetoothDevice source = Build.VERSION.SDK_INT >= 33
                    ? intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice.class)
                    : intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
            if (!device.equals(source)) return;
            handler.post(() -> {
                if (closed) return;
                try {
                    if (Build.VERSION.SDK_INT >= 36 && BluetoothDevice.ACTION_ENCRYPTION_CHANGE.equals(intent.getAction())) {
                        if (intent.getIntExtra(BluetoothDevice.EXTRA_TRANSPORT, -1) != BluetoothDevice.TRANSPORT_LE) return;
                        encrypted = intent.getBooleanExtra(BluetoothDevice.EXTRA_ENCRYPTION_ENABLED, false)
                                && intent.getIntExtra(BluetoothDevice.EXTRA_ENCRYPTION_STATUS, -1) == 0;
                        if (!encrypted) { abort("Criptografia Bluetooth ausente ou perdida"); return; }
                    } else if (device.getBondState() == BluetoothDevice.BOND_NONE) {
                        abort("Pareamento cancelado; use o PIN do portal ESP32"); return;
                    }
                    secureSubscribe();
                } catch (RuntimeException e) { abort("Permissão Bluetooth revogada"); }
            });
        }
    };

    private void secureSubscribe() {
        if (closed || !discovered || subscribing) return;
        int bond = device.getBondState();
        if (bond == BluetoothDevice.BOND_NONE) {
            if (!device.createBond()) abort("Não foi possível abrir pareamento Android");
            return;
        }
        if (bond != BluetoothDevice.BOND_BONDED || (Build.VERSION.SDK_INT >= 36 && !encrypted)) return;
        subscribing = true;
        if (!gatt.setCharacteristicNotification(tx, true)) { abort("Notificações indisponíveis"); return; }
        enqueue(new Operation(Kind.SUBSCRIBE, () -> {
            if (Build.VERSION.SDK_INT >= 33) return gatt.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                    == BluetoothStatusCodes.SUCCESS;
            cccd.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
            return gatt.writeDescriptor(cccd);
        }, () -> {
            ready = true; handler.removeCallbacks(startupTimeout); listener.ready();
            while (!earlyNotifications.isEmpty() && !closed) listener.frame(earlyNotifications.remove());
        }, () -> true, new CompletableFuture<>()));
    }

    private void enqueue(Operation op) {
        if (closed) { op.result.completeExceptionally(new CancellationException()); return; }
        if (queue.size() >= 80) { op.result.completeExceptionally(new IllegalStateException("Fila BLE cheia")); abort("Fila BLE excedida"); return; }
        queue.add(op); pump();
    }

    private void pump() {
        if (closed || active != null) return;
        while (!queue.isEmpty()) {
            Operation op = queue.remove();
            if (!op.valid.getAsBoolean()) { op.result.completeExceptionally(new CancellationException()); continue; }
            active = op;
            handler.postDelayed(operationTimeout, 10_000);
            try { if (!op.start.run()) abort("Operação GATT recusada"); }
            catch (RuntimeException e) { abort("Falha GATT ou permissão revogada"); }
            return;
        }
    }

    private void finish(Kind kind, int status) {
        if (closed) return;
        if (active == null || active.kind != kind || status != BluetoothGatt.GATT_SUCCESS) {
            abort("Callback GATT inesperado ou erro de segurança"); return;
        }
        Operation completed = active; active = null; handler.removeCallbacks(operationTimeout);
        try { completed.done.run(); completed.result.complete(null); }
        catch (RuntimeException e) { completed.result.completeExceptionally(e); abort("Falha ao processar GATT"); }
        pump();
    }

    @Override public CompletableFuture<Void> send(byte[] frame, BooleanSupplier valid) {
        CompletableFuture<Void> future = new CompletableFuture<>();
        byte[] copy = frame.clone();
        if (!handler.post(() -> {
            if (!ready || closed || copy.length > Frame.MAX_FRAME) {
                future.completeExceptionally(new IllegalStateException("BLE indisponível")); return;
            }
            enqueue(new Operation(Kind.WRITE, () -> {
                if (Build.VERSION.SDK_INT >= 33) return gatt.writeCharacteristic(rx, copy,
                        BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS;
                rx.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT); rx.setValue(copy);
                return gatt.writeCharacteristic(rx);
            }, () -> { }, valid, future));
        })) future.completeExceptionally(new CancellationException());
        return future;
    }

    private final BluetoothGattCallback callback = new BluetoothGattCallback() {
        private boolean current(BluetoothGatt source) { return !closed && source == gatt; }
        @Override public void onConnectionStateChange(BluetoothGatt source, int status, int state) {
            if (!current(source)) return;
            if (status != BluetoothGatt.GATT_SUCCESS || state == BluetoothProfile.STATE_DISCONNECTED) {
                abort("Bluetooth desconectado"); return;
            }
            if (state != BluetoothProfile.STATE_CONNECTED) return;
            listener.connected();
            enqueue(new Operation(Kind.MTU, () -> gatt.requestMtu(183), () ->
                    enqueue(new Operation(Kind.DISCOVER, () -> gatt.discoverServices(), () -> {
                        discovered = true; secureSubscribe();
                    }, () -> true, new CompletableFuture<>())), () -> true, new CompletableFuture<>()));
        }
        @Override public void onMtuChanged(BluetoothGatt source, int mtu, int status) {
            if (!current(source)) return;
            if (mtu < 183) { abort("MTU abaixo de 183; relay indisponível"); return; }
            finish(Kind.MTU, status);
        }
        @Override public void onServicesDiscovered(BluetoothGatt source, int status) {
            if (!current(source)) return;
            BluetoothGattService service = source.getService(SERVICE);
            if (service == null) { abort("Serviço IdeiaRec ausente"); return; }
            rx = service.getCharacteristic(RX); tx = service.getCharacteristic(TX);
            if (rx == null || tx == null || (rx.getProperties() & BluetoothGattCharacteristic.PROPERTY_WRITE) == 0
                    || (tx.getProperties() & BluetoothGattCharacteristic.PROPERTY_NOTIFY) == 0
                    || (cccd = tx.getDescriptor(CCCD)) == null) {
                abort("Características GATT incompatíveis"); return;
            }
            finish(Kind.DISCOVER, status);
        }
        @Override public void onDescriptorWrite(BluetoothGatt source, BluetoothGattDescriptor descriptor, int status) {
            if (current(source)) {
                if (descriptor != cccd) abort("Descritor GATT inesperado");
                else finish(Kind.SUBSCRIBE, status);
            }
        }
        @Override public void onCharacteristicWrite(BluetoothGatt source, BluetoothGattCharacteristic characteristic, int status) {
            if (current(source)) {
                if (characteristic != rx) abort("Write GATT inesperado");
                else finish(Kind.WRITE, status);
            }
        }
        private void notification(BluetoothGatt source, BluetoothGattCharacteristic characteristic, byte[] value) {
            if (!current(source) || !characteristic.getUuid().equals(TX)) return;
            if (value == null || value.length > Frame.MAX_FRAME) { abort("Frame inválido"); return; }
            byte[] copy = value.clone();
            if (ready) listener.frame(copy);
            else if (subscribing && earlyNotifications.size() < 8) earlyNotifications.add(copy);
            else abort("Notificação fora da sessão");
        }
        @Override public void onCharacteristicChanged(BluetoothGatt source, BluetoothGattCharacteristic characteristic, byte[] value) {
            notification(source, characteristic, value);
        }
        @Override public void onCharacteristicChanged(BluetoothGatt source, BluetoothGattCharacteristic characteristic) {
            if (Build.VERSION.SDK_INT < 33) notification(source, characteristic, characteristic.getValue());
        }
    };

    private void abort(String status) {
        if (closed) return;
        close(); listener.failed(status);
    }

    /** Chamado no handler proprietário, inclusive em timeout; nenhum GATT é reutilizado. */
    @Override public void close() {
        if (closed) return;
        closed = true; ready = false;
        handler.removeCallbacks(operationTimeout); handler.removeCallbacks(startupTimeout);
        if (registered) { context.unregisterReceiver(security); registered = false; }
        CancellationException cancelled = new CancellationException("Sessão encerrada");
        if (active != null) { active.result.completeExceptionally(cancelled); active = null; }
        while (!queue.isEmpty()) queue.remove().result.completeExceptionally(cancelled);
        earlyNotifications.clear();
        if (gatt != null) {
            try { gatt.disconnect(); } catch (RuntimeException ignored) { }
            try { gatt.close(); } catch (RuntimeException ignored) { }
            gatt = null;
        }
    }
}
