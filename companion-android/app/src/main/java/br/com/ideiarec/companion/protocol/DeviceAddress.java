package br.com.ideiarec.companion.protocol;

import java.util.Locale;

/** CDM/MacAddress usa minúsculas; BluetoothAdapter requer a forma canônica. */
public final class DeviceAddress {
    private DeviceAddress() { }
    public static String normalize(String value) {
        if (value == null) throw new IllegalArgumentException("Endereço Bluetooth ausente");
        String canonical = value.trim().toUpperCase(Locale.ROOT);
        if (!canonical.matches("(?:[0-9A-F]{2}:){5}[0-9A-F]{2}")) {
            throw new IllegalArgumentException("Endereço Bluetooth inválido");
        }
        return canonical;
    }
}
