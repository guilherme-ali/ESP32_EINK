package br.com.ideiarec.companion;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.PendingIntent;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanSettings;
import android.companion.AssociationInfo;
import android.companion.CompanionDeviceManager;
import android.companion.ObservingDevicePresenceRequest;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.ParcelUuid;
import java.util.Collections;
import java.util.List;

final class Presence {
    static final String ACTION_BLE_FOUND = "br.com.ideiarec.companion.ACTION_BLE_FOUND";
    private Presence() { }

    private static PendingIntent scanIntent(Context context) {
        Intent intent = new Intent(context, RestoreReceiver.class).setAction(ACTION_BLE_FOUND);
        int flags = PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE;
        return PendingIntent.getBroadcast(context, 7, intent, flags);
    }

    @SuppressLint("MissingPermission")
    private static void updateScanner(Context context, Config c, boolean start) {
        if (context.checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED) return;
        BluetoothManager bt = context.getSystemService(BluetoothManager.class);
        BluetoothAdapter adapter = bt == null ? null : bt.getAdapter();
        BluetoothLeScanner scanner = adapter != null && adapter.isEnabled() ? adapter.getBluetoothLeScanner() : null;
        if (scanner == null) return;
        PendingIntent pending = scanIntent(context);
        try { scanner.stopScan(pending); } catch (RuntimeException ignored) { }
        if (!start) return;
        List<ScanFilter> filters = Collections.singletonList(new ScanFilter.Builder()
                .setDeviceAddress(c.mac()).setServiceUuid(new ParcelUuid(GattTransport.SERVICE)).build());
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .setCallbackType(ScanSettings.CALLBACK_TYPE_ALL_MATCHES)
                .setMatchMode(ScanSettings.MATCH_MODE_AGGRESSIVE).build();
        scanner.startScan(filters, settings, pending);
    }

    @SuppressLint("MissingPermission")
    @SuppressWarnings("deprecation")
    static void observe(Context context, boolean start) {
        Config c = new Config(context);
        if (c.mac().isEmpty()) return;
        CompanionDeviceManager manager = context.getSystemService(CompanionDeviceManager.class);
        if (manager == null) throw new IllegalStateException("CDM indisponível");
        if (Build.VERSION.SDK_INT >= 36) {
            int id = c.associationId();
            if (id < 0) {
                for (AssociationInfo a : manager.getMyAssociations()) {
                    if (a.getDeviceMacAddress() != null && c.mac().equalsIgnoreCase(a.getDeviceMacAddress().toString())) {
                        id = a.getId(); c.associationId(id); break;
                    }
                }
            }
            if (id < 0) throw new IllegalStateException("Reassocie o dispositivo");
            ObservingDevicePresenceRequest request = new ObservingDevicePresenceRequest.Builder().setAssociationId(id).build();
            if (start) manager.startObservingDevicePresence(request);
            else manager.stopObservingDevicePresence(request);
        } else {
            if (start) manager.startObservingDevicePresence(c.mac());
            else manager.stopObservingDevicePresence(c.mac());
        }
        updateScanner(context, c, start);
    }
}
