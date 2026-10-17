package br.com.ideiarec.companion;

import android.Manifest;
import android.annotation.SuppressLint;
import android.app.Activity;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.le.ScanFilter;
import android.bluetooth.le.ScanResult;
import android.companion.AssociationInfo;
import android.companion.AssociationRequest;
import android.companion.BluetoothLeDeviceFilter;
import android.companion.CompanionDeviceManager;
import android.content.Intent;
import android.content.IntentSender;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import android.view.WindowInsets;
import br.com.ideiarec.companion.protocol.HostPolicy;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Set;
import java.util.TreeSet;

public final class MainActivity extends Activity {
    private static final int PAIR = 41, PERMISSIONS = 42;
    private Config config;
    private TextView status;
    private EditText hosts;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            status.setText(getString(R.string.status_format,
                    config.mac().isEmpty() ? getString(R.string.no_association) : config.mac(),
                    getString(config.enabled() ? R.string.enabled : R.string.disabled), config.status(),
                    getString(RelayService.running ? R.string.service_active : R.string.service_inactive)));
            ui.postDelayed(this, 1000);
        }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        config = new Config(this);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        content.setPadding(pad, pad, pad, pad);
        ScrollView scroll = new ScrollView(this); scroll.addView(content); setContentView(scroll);
        scroll.setOnApplyWindowInsetsListener((view, insets) -> {
            android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.ime());
            view.setPadding(bars.left, bars.top, bars.right, bars.bottom); return insets;
        });
        TextView title = new TextView(this); title.setText(R.string.title); title.setTextSize(23); content.addView(title);
        status = new TextView(this); content.addView(status);
        TextView help = new TextView(this);
        help.setText(R.string.pairing_help);
        content.addView(help);
        button(content, "Permissões", this::permissions);
        button(content, "Parear / associar IdeiaRec", this::associate);
        button(content, "Reassociar (refazer CDM)", this::reassociate);
        button(content, "Iniciar / observar presença", this::startRelay);
        button(content, "Parar e desativar automático", this::stopRelay);
        TextView label = new TextView(this); label.setText(R.string.host_label); content.addView(label);
        hosts = new EditText(this);
        hosts.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE
                | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        hosts.setMinLines(9); hosts.setText(String.join("\n", new TreeSet<>(config.hosts()))); content.addView(hosts);
        button(content, "Salvar hosts e reiniciar sessão", this::saveHosts);
    }

    private void button(LinearLayout content, String text, Runnable action) {
        Button b = new Button(this); b.setText(text); b.setOnClickListener(v -> action.run()); content.addView(b);
    }
    private void message(String text) { Toast.makeText(this, text, Toast.LENGTH_LONG).show(); }

    private boolean bluetoothPermissions() {
        return checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
    }
    private void permissions() {
        ArrayList<String> needed = new ArrayList<>();
        for (String p : new String[]{Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT}) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) needed.add(p);
        }
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
            needed.add(Manifest.permission.POST_NOTIFICATIONS);
        }
        if (!needed.isEmpty()) requestPermissions(needed.toArray(new String[0]), PERMISSIONS);
        else message("Permissões concedidas");
    }

    @SuppressWarnings("deprecation")
    private void associate() {
        if (!bluetoothPermissions()) { permissions(); return; }
        if (!config.mac().isEmpty()) { message("Já associado; use Iniciar ou Reassociar"); return; }
        BluetoothLeDeviceFilter filter = new BluetoothLeDeviceFilter.Builder()
                .setScanFilter(new ScanFilter.Builder().setServiceUuid(new ParcelUuid(GattTransport.SERVICE)).build()).build();
        AssociationRequest request = new AssociationRequest.Builder().addDeviceFilter(filter).setSingleDevice(false).build();
        try {
            getSystemService(CompanionDeviceManager.class).associate(request, new CompanionDeviceManager.Callback() {
                private void launch(IntentSender sender) {
                    try { startIntentSenderForResult(sender, PAIR, null, 0, 0, 0); }
                    catch (IntentSender.SendIntentException e) { message("Não foi possível abrir associação"); }
                }
                @Override public void onDeviceFound(IntentSender sender) { launch(sender); }
                @Override public void onAssociationPending(IntentSender sender) { launch(sender); }
                @Override public void onAssociationCreated(AssociationInfo info) {
                    if (Build.VERSION.SDK_INT >= 33 && info.getDeviceMacAddress() != null) {
                        config.associate(info.getDeviceMacAddress().toString(), info.getId()); startRelay();
                    }
                }
                @Override public void onFailure(CharSequence ignored) { message("Associação cancelada ou indisponível"); }
            }, ui);
        } catch (RuntimeException e) { message("CDM indisponível; verifique Bluetooth e permissões"); }
    }

    @SuppressWarnings("deprecation")
    @SuppressLint("MissingPermission")
    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PAIR || result != RESULT_OK || data == null || Build.VERSION.SDK_INT >= 33) return;
        Object selected = data.getParcelableExtra(CompanionDeviceManager.EXTRA_DEVICE);
        BluetoothDevice device = selected instanceof ScanResult ? ((ScanResult) selected).getDevice()
                : selected instanceof BluetoothDevice ? (BluetoothDevice) selected : null;
        if (device != null) { config.associate(device.getAddress(), -1); startRelay(); }
    }

    private void startRelay() {
        if (!bluetoothPermissions()) { permissions(); return; }
        if (config.mac().isEmpty()) { message("Associe o IdeiaRec primeiro"); return; }
        config.enabled(true);
        try {
            Presence.observe(this, true);
            startForegroundService(new Intent(this, RelayService.class));
        } catch (RuntimeException e) { config.status("Revise associação e permissões Bluetooth"); }
    }
    private void stopRelay() {
        config.enabled(false);
        try { Presence.observe(this, false); } catch (RuntimeException ignored) { }
        stopService(new Intent(this, RelayService.class)); config.status("Parado pelo usuário");
    }
    @SuppressWarnings("deprecation")
    private void reassociate() {
        stopRelay();
        try {
            CompanionDeviceManager cdm = getSystemService(CompanionDeviceManager.class);
            if (Build.VERSION.SDK_INT >= 33 && config.associationId() >= 0) cdm.disassociate(config.associationId());
            else if (!config.mac().isEmpty()) cdm.disassociate(config.mac());
            config.forget(); associate();
        } catch (RuntimeException e) { message("Falha ao remover associação; revise nas configurações Android"); }
    }
    private void saveHosts() {
        try {
            Set<String> validated = new HashSet<>();
            for (String h : hosts.getText().toString().split("\\R")) if (!h.trim().isEmpty()) validated.add(HostPolicy.normalize(h.trim()));
            new HostPolicy(validated); config.hosts(validated);
            boolean resume = config.enabled();
            stopService(new Intent(this, RelayService.class));
            if (resume) startRelay();
            message("Hosts salvos; conexões antigas encerradas");
        } catch (IllegalArgumentException e) { message("Use somente domínios DNS exatos, sem URL, IP, porta ou curinga"); }
    }
    @SuppressWarnings("deprecation")
    private void restoreAssociation() {
        if (!bluetoothPermissions() || !config.mac().isEmpty()) return;
        try {
            CompanionDeviceManager manager = getSystemService(CompanionDeviceManager.class);
            if (Build.VERSION.SDK_INT >= 33) {
                for (AssociationInfo a : manager.getMyAssociations()) {
                    if (a.getDeviceMacAddress() != null) {
                        config.associate(a.getDeviceMacAddress().toString(), a.getId()); break;
                    }
                }
            } else if (!manager.getAssociations().isEmpty()) config.associate(manager.getAssociations().get(0), -1);
            if (!config.mac().isEmpty()) Presence.observe(this, true);
        } catch (RuntimeException e) { config.status("Revise associação e permissões"); }
    }
    @Override protected void onResume() {
        super.onResume(); restoreAssociation(); ui.removeCallbacks(refresh); ui.post(refresh);
    }
    @Override protected void onPause() { ui.removeCallbacks(refresh); super.onPause(); }
}
