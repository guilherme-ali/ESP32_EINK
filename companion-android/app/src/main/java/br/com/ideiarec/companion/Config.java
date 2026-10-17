package br.com.ideiarec.companion;

import android.content.Context;
import android.content.SharedPreferences;
import br.com.ideiarec.companion.protocol.HostPolicy;
import br.com.ideiarec.companion.protocol.DeviceAddress;
import java.util.HashSet;
import java.util.Set;

final class Config {
    private final SharedPreferences prefs;
    Config(Context context) { prefs = context.getSharedPreferences("companion", Context.MODE_PRIVATE); }
    String mac() {
        String value = prefs.getString("mac", "");
        if (value.isEmpty()) return value;
        try { return DeviceAddress.normalize(value); }
        catch (IllegalArgumentException ignored) { return ""; }
    }
    int associationId() { return prefs.getInt("association", -1); }
    void associationId(int id) { prefs.edit().putInt("association", id).apply(); }
    boolean enabled() { return prefs.getBoolean("enabled", false); }
    void enabled(boolean value) { prefs.edit().putBoolean("enabled", value).apply(); }
    void associate(String mac, int id) {
        prefs.edit().putString("mac", DeviceAddress.normalize(mac)).putInt("association", id).putBoolean("enabled", true).apply();
    }
    void forget() { prefs.edit().remove("mac").remove("association").putBoolean("enabled", false).apply(); }
    Set<String> hosts() { return new HashSet<>(prefs.getStringSet("hosts", HostPolicy.DEFAULTS)); }
    void hosts(Set<String> hosts) { prefs.edit().putStringSet("hosts", new HashSet<>(hosts)).apply(); }
    String status() { return prefs.getString("status", "Parado"); }
    void status(String status) { prefs.edit().putString("status", status).apply(); }
}
