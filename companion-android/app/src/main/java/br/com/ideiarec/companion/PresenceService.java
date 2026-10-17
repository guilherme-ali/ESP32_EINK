package br.com.ideiarec.companion;

import android.companion.AssociationInfo;
import android.companion.CompanionDeviceService;
import android.companion.DevicePresenceEvent;
import android.content.Intent;
import android.os.Build;

public final class PresenceService extends CompanionDeviceService {
    private boolean connected;

    private boolean matches(String address) { return new Config(this).mac().equalsIgnoreCase(address); }

    private void appeared() {
        Config c = new Config(this);
        if (!c.enabled() || c.mac().isEmpty()) return;
        try { startForegroundService(new Intent(this, RelayService.class)); }
        catch (RuntimeException e) { c.status("Início em background recusado; abra o app"); }
    }

    private void disappeared() { stopService(new Intent(this, RelayService.class)); }

    @Override public void onDevicePresenceEvent(DevicePresenceEvent event) {
        if (Build.VERSION.SDK_INT < 36 || event.getAssociationId() != new Config(this).associationId()) return;
        switch (event.getEvent()) {
            case DevicePresenceEvent.EVENT_BT_CONNECTED:
                connected = true; appeared(); break;
            case DevicePresenceEvent.EVENT_BLE_APPEARED:
                appeared(); break;
            case DevicePresenceEvent.EVENT_BT_DISCONNECTED:
                connected = false; disappeared(); break;
            case DevicePresenceEvent.EVENT_BLE_DISAPPEARED:
                if (!connected && !RelayService.linkConnected) disappeared();
                break;
            default: break;
        }
    }

    @Override public void onDeviceAppeared(AssociationInfo association) {
        if (Build.VERSION.SDK_INT >= 33 && association.getDeviceMacAddress() != null
                && matches(association.getDeviceMacAddress().toString())) appeared();
    }
    @Override public void onDeviceDisappeared(AssociationInfo association) {
        if (Build.VERSION.SDK_INT >= 33 && association.getDeviceMacAddress() != null && matches(association.getDeviceMacAddress().toString())
                && !RelayService.linkConnected) disappeared();
    }
    @SuppressWarnings("deprecation")
    @Override public void onDeviceAppeared(String address) { if (matches(address)) appeared(); }
    @SuppressWarnings("deprecation")
    @Override public void onDeviceDisappeared(String address) {
        if (matches(address) && !RelayService.linkConnected) disappeared();
    }
}
