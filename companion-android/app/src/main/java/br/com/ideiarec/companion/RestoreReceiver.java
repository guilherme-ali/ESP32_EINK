package br.com.ideiarec.companion;

import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanResult;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import java.util.ArrayList;

/** Reboot/package update apenas registra presença. Scan BLE inicia RelayService sob demanda. */
public final class RestoreReceiver extends BroadcastReceiver {
    @SuppressWarnings("deprecation")
    @Override public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();
        Config c = new Config(context);
        if (!c.enabled() || c.mac().isEmpty()) return;
        if (Presence.ACTION_BLE_FOUND.equals(action)) {
            if (RelayService.running) return;
            ArrayList<ScanResult> results = Build.VERSION.SDK_INT >= 33
                    ? intent.getParcelableArrayListExtra(BluetoothLeScanner.EXTRA_LIST_SCAN_RESULT, ScanResult.class)
                    : intent.getParcelableArrayListExtra(BluetoothLeScanner.EXTRA_LIST_SCAN_RESULT);
            if (results == null) return;
            for (ScanResult r : results) {
                if (r != null && r.getDevice() != null && c.mac().equalsIgnoreCase(r.getDevice().getAddress())) {
                    try { context.startForegroundService(new Intent(context, RelayService.class)); }
                    catch (RuntimeException e) { c.status("Início por scan recusado; abra o app"); }
                    return;
                }
            }
            return;
        }
        if (!Intent.ACTION_BOOT_COMPLETED.equals(action) && !Intent.ACTION_MY_PACKAGE_REPLACED.equals(action)) return;
        try { Presence.observe(context, true); c.status("Aguardando presença do IdeiaRec"); }
        catch (RuntimeException e) { c.status("Abra o app para revisar associação/permissões"); }
    }
}
