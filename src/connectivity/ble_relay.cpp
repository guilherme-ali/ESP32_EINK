#if defined(ARDUINO_ARCH_ESP32)

#include "ble_relay.h"
#include "relay_protocol.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEService.h>
#include <NimBLECharacteristic.h>
#include <NimBLEConnInfo.h>
#if defined(USING_NIMBLE_ARDUINO_HEADERS)
#include <nimble/nimble/host/include/host/ble_store.h>
#else
#include <host/ble_store.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstring>

// APIs verificadas no tag oficial 2.5.1. O coordenador fixa a dependência.
#if NIMBLE_CPP_VERSION_MAJOR != 2
#error "PhoneRelay requer NimBLE-Arduino 2.x (APIs verificadas em 2.5.1)"
#endif

namespace PhoneRelay {
namespace {
namespace P = Protocol;
constexpr char kService[] = "7fd00001-6f6e-4d69-9b18-8e8c49b5c100";
constexpr char kRx[] = "7fd00002-6f6e-4d69-9b18-8e8c49b5c100";
constexpr char kTx[] = "7fd00003-6f6e-4d69-9b18-8e8c49b5c100";
constexpr uint32_t kMaxWaitMs = 60000;
constexpr uint32_t kAckTimeoutMs = 30000;
constexpr size_t kQueueSize = P::kChannels * (2 * P::kWindow + 4);

portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
// Nunca se mantém stateMux durante chamadas NimBLE, sleeps ou espera de crédito.
struct StateLock {
    StateLock() { portENTER_CRITICAL(&stateMux); }
    ~StateLock() { portEXIT_CRITICAL(&stateMux); }
    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;
};

struct LocalChannel {
    P::Channel protocol;
    uint32_t lastProgress = 0;
    bool writing = false;
    TaskHandle_t writerTask = nullptr;
};
struct Queued {
    P::Frame frame;
    uint32_t link = 0;
    uint32_t generation = 0;
};

LocalChannel channels[P::kChannels];
Queued queue[kQueueSize];
size_t queueHead = 0;
size_t queueCount = 0;
uint16_t phone = BLE_HS_CONN_HANDLE_NONE;
uint16_t mtu = 23;
uint32_t linkGeneration = 0;
bool enabled = false;
bool authenticated = false;
bool subscribed = false;
bool disconnectPending = false;
bool radioStopPending = false;

std::atomic<const char*> errorText{""};
std::atomic<uint32_t> pairingPin{0};
std::atomic<int> securityResult{0}, bondResult{0}, disconnectReason{0}, authenticationFlags{0};
std::atomic<uint32_t> passkeyRequests{0};
std::atomic<uint16_t> interval1250us{0}, latency{0}, supervisionTimeout10ms{0};
std::atomic<uint16_t> maxTxOctets{0}, maxRxOctets{0}, maxTxTime{0}, maxRxTime{0};
std::atomic<uint8_t> txPhy{0}, rxPhy{0};
std::atomic_flag lifecycleBusy = ATOMIC_FLAG_INIT;
std::atomic<bool> workerExit{false};
std::atomic<bool> workerExited{false};
TaskHandle_t workerHandle = nullptr; // apenas lifecycle altera
StaticSemaphore_t ioStorage;
SemaphoreHandle_t ioMutex = nullptr; // criado antes do worker/callbacks
NimBLEServer* server = nullptr;       // lifecycle/ioMutex, callbacks não alteram
NimBLECharacteristic* tx = nullptr;
bool initialized = false;            // apenas lifecycle
bool initializationFailed = false;   // requer shutdown antes de tentar novamente
bool finalShutdown = false;          // shutdown é terminal; evita reinit inseguro

void setError(const char* text) { errorText.store(text, std::memory_order_release); }
bool validChannel(int channel) { return channel >= 0 && channel < P::kChannels; }
bool readyLocked() {
    return enabled && !disconnectPending && phone != BLE_HS_CONN_HANDLE_NONE &&
           authenticated && subscribed && mtu >= P::kMinMtu;
}
bool secure(const NimBLEConnInfo& info) {
    return info.isEncrypted() && info.isAuthenticated() && info.isBonded() && info.getSecKeySize() == 16;
}
bool secureBond(const NimBLEConnInfo& info) {
    // sm_sc_only controla pareamentos novos. Não aceitar um bond legado que
    // tenha restado no NVS de outro firmware, mesmo com MITM e chave de 128 bits.
    if (!info.isEncrypted()) { setError("BLE criptografia ausente"); return false; }
    if (!info.isAuthenticated()) { setError("BLE MITM ausente"); return false; }
    if (!info.isBonded()) { setError("BLE bond ausente"); return false; }
    if (info.getSecKeySize() != 16) { setError("BLE chave nao tem 128 bits"); return false; }
    ble_store_key_sec key{};
    key.peer_addr = *info.getIdAddress().getBase();
    ble_store_value_sec bond{};
    const int stored = ble_store_read_our_sec(&key, &bond);
    bondResult.store(stored);
    if (stored != 0) { setError("BLE bond persistido nao encontrado"); return false; }
    const bool ok = bond.sc && bond.authenticated && bond.ltk_present && bond.key_size == 16;
    if (!ok) setError("BLE bond persistido sem SC MITM");
    // O valor de segurança é local ao callback; não fica em estado compartilhado.
    volatile uint8_t* bytes = reinterpret_cast<volatile uint8_t*>(&bond);
    for (size_t i = 0; i < sizeof(bond); ++i) bytes[i] = 0;
    return ok;
}
uint32_t bounded(uint32_t ms) { return ms < kMaxWaitMs ? ms : kMaxWaitMs; }
bool expired(uint32_t start, uint32_t timeout) { return static_cast<uint32_t>(millis() - start) >= timeout; }
void pause() { vTaskDelay(1); }
void wakeWorkerLocked() { if (workerHandle) xTaskNotifyGive(workerHandle); }
void wakeWriterLocked(LocalChannel& channel) {
    if (channel.writerTask) xTaskNotifyGive(channel.writerTask);
    channel.writerTask = nullptr;
    channel.writing = false;
}

struct Lifecycle {
    bool held;
    Lifecycle() : held(!lifecycleBusy.test_and_set(std::memory_order_acquire)) {
        if (!held) setError("Lifecycle BLE ocupado");
    }
    ~Lifecycle() { if (held) lifecycleBusy.clear(std::memory_order_release); }
};

void clearQueueLocked() {
    // Apaga também bytes HTTP que já saíram da fila lógica.
    for (auto& item : queue) item = Queued{};
    queueHead = queueCount = 0;
}
void failLinkLocked(const char* reason) {
    setError(reason);
    authenticated = false;
    subscribed = false;
    disconnectPending = true;
    radioStopPending = true;
    ++linkGeneration;
    clearQueueLocked();
    for (auto& channel : channels) {
        wakeWriterLocked(channel);
        if (channel.protocol.state() != P::State::Idle) channel.protocol.abort();
    }
    wakeWorkerLocked();
}

bool enqueueLocked(const P::Frame& frame) {
    if (!readyLocked()) return false;
    if (queueCount == kQueueSize) {
        failLinkLocked("Fila BLE esgotada");
        return false;
    }
    Queued& item = queue[(queueHead + queueCount) % kQueueSize];
    item.frame = frame;
    item.link = linkGeneration;
    item.generation = channels[frame.channel].protocol.generation();
    ++queueCount;
    wakeWorkerLocked();
    return true;
}
P::Frame controlLocked(int channel, P::Type type) {
    P::Frame frame;
    frame.type = type;
    frame.channel = static_cast<uint8_t>(channel);
    frame.epoch = channels[channel].protocol.epoch();
    return frame;
}
void abortChannelLocked(int channel, const char* reason, bool report = true) {
    LocalChannel& c = channels[channel];
    if (c.protocol.state() == P::State::Idle) return;
    c.protocol.abort();
    wakeWriterLocked(c);
    setError(reason);
    if (report && readyLocked()) {
        P::Frame frame = controlLocked(channel, P::Type::Error);
        // Mensagem fixa, nunca ecoa host, respostas, credenciais ou PIN.
        constexpr char message[] = "channel aborted";
        frame.length = sizeof(message) - 1;
        std::memcpy(frame.payload, message, frame.length);
        enqueueLocked(frame);
    }
}
bool ackLocked(int channel, uint16_t sequence) {
    P::Frame ack = controlLocked(channel, P::Type::Ack);
    ack.seq = sequence;
    if (enqueueLocked(ack)) return true;
    if (channels[channel].protocol.state() != P::State::Aborted) {
        abortChannelLocked(channel, "ACK BLE indisponivel", false);
    }
    return false;
}
void drainEmptyLocked(int channel) {
    for (uint8_t i = 0; i < P::kWindow; ++i) {
        const P::Consumption consumed = channels[channel].protocol.consume(nullptr, 0);
        if (!consumed.ack || !ackLocked(channel, consumed.sequence)) break;
    }
}

int pairingDiagnostic(ble_gap_event* event, void*) {
    if (event->type == BLE_GAP_EVENT_ENC_CHANGE) securityResult.store(event->enc_change.status);
    if (event->type == BLE_GAP_EVENT_PASSKEY_ACTION) passkeyRequests.fetch_add(1);
    if (event->type == BLE_GAP_EVENT_DATA_LEN_CHG) {
        bool current;
        { StateLock lock; current = phone == event->data_len_chg.conn_handle; }
        if (current) {
            maxTxOctets.store(event->data_len_chg.max_tx_octets);
            maxRxOctets.store(event->data_len_chg.max_rx_octets);
            maxTxTime.store(event->data_len_chg.max_tx_time);
            maxRxTime.store(event->data_len_chg.max_rx_time);
        }
    }
    return 0;
}

class ServerCallbacks final : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        bool accept;
        const uint16_t connectedMtu = info.getMTU(); // API host fora de stateMux
        const uint16_t interval = info.getConnInterval(), peerLatency = info.getConnLatency();
        const uint16_t supervision = info.getConnTimeout();
        {
            StateLock lock;
            accept = enabled && !disconnectPending && phone == BLE_HS_CONN_HANDLE_NONE;
            if (accept) {
                phone = info.getConnHandle();
                mtu = connectedMtu;
                interval1250us.store(interval); latency.store(peerLatency);
                supervisionTimeout10ms.store(supervision);
                authenticated = false; // só onAuthenticationComplete valida o bond SC
                subscribed = false;
                ++linkGeneration;
            }
        }
        if (!accept) {
            s->disconnect(info.getConnHandle());
            return;
        }
        NimBLEDevice::stopAdvertising();
        // Primeiro vínculo: Android chama createBond após descobrir os serviços.
        // Evita dois iniciadores SMP concorrentes. No retorno de um vínculo já
        // salvo, o periférico solicita criptografia para acordar a sessão segura.
        if (NimBLEDevice::isBonded(info.getIdAddress())) {
            int rc = 0;
            const bool securing = NimBLEDevice::startSecurity(info.getConnHandle(), &rc);
            securityResult.store(rc);
            if (!securing && rc != BLE_HS_EALREADY) {
                StateLock lock;
                if (phone == info.getConnHandle()) failLinkLocked("Pareamento BLE falhou");
            }
        }
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
        disconnectReason.store(reason);
        StateLock lock;
        if (phone != info.getConnHandle()) return; // segundo telefone rejeitado
        if (enabled && !errorText.load(std::memory_order_acquire)[0]) setError("Telefone desconectado");
        phone = BLE_HS_CONN_HANDLE_NONE;
        mtu = 23;
        interval1250us = latency = supervisionTimeout10ms = 0;
        maxTxOctets = maxRxOctets = maxTxTime = maxRxTime = 0;
        txPhy = rxPhy = 0;
        authenticated = subscribed = false;
        disconnectPending = false;
        ++linkGeneration;
        clearQueueLocked();
        for (auto& channel : channels) {
            wakeWriterLocked(channel);
            if (channel.protocol.state() != P::State::Idle) channel.protocol.abort();
        }
        // Sem reanúncio automático: somente begin() solicita rádio novamente.
    }
    void onMTUChange(uint16_t newMtu, NimBLEConnInfo& info) override {
        StateLock lock;
        if (phone != info.getConnHandle()) return;
        const bool wasReady = readyLocked();
        mtu = newMtu;
        if (wasReady && newMtu < P::kMinMtu) failLinkLocked("MTU BLE insuficiente");
    }
    void onConnParamsUpdate(NimBLEConnInfo& info) override {
        StateLock lock;
        if (phone != info.getConnHandle()) return;
        interval1250us.store(info.getConnInterval()); latency.store(info.getConnLatency());
        supervisionTimeout10ms.store(info.getConnTimeout());
    }
    void onPhyUpdate(NimBLEConnInfo& info, uint8_t tx, uint8_t rx) override {
        StateLock lock;
        if (phone != info.getConnHandle()) return;
        txPhy.store(tx); rxPhy.store(rx);
    }
    uint32_t onPassKeyDisplay() override {
        return pairingPin.load(std::memory_order_acquire);
    }
    void onConfirmPassKey(NimBLEConnInfo& info, uint32_t) override {
        // DisplayOnly deve negociar Passkey Entry, não confirmar comparações.
        NimBLEDevice::injectConfirmPasskey(info, false);
    }
    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        authenticationFlags.store((info.isEncrypted() ? 1 : 0) | (info.isAuthenticated() ? 2 : 0) |
                                  (info.isBonded() ? 4 : 0) | (static_cast<int>(info.getSecKeySize()) << 8));
        const bool trusted = secureBond(info); // consulta NVS fora de stateMux
        {
            StateLock lock;
            if (phone != info.getConnHandle()) return;
            authenticated = trusted;
            if (!authenticated) failLinkLocked(errorText.load(std::memory_order_acquire));
        }
        if (trusted) {
            // Negociar desempenho depois de concluir SMP, fora do stateMux.
            server->updateConnParams(info.getConnHandle(), 6, 12, 0, 400);
            server->setDataLen(info.getConnHandle(), 251);
            server->updatePhy(info.getConnHandle(), BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
        }
    }
};

class CharacteristicCallbacks final : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo& info, uint16_t value) override {
        StateLock lock;
        if (phone != info.getConnHandle()) return;
        const bool wasReady = readyLocked();
        subscribed = (value & 1) != 0;
        if (wasReady && !subscribed) failLinkLocked("Notificacoes BLE canceladas");
    }
    void onStatus(NimBLECharacteristic*, int code) override {
        // O tag 2.5.1 também chama o overload novo com peerInfo sem preencher
        // o handle neste evento. O overload antigo é necessário neste tag.
        if (!code) return;
        StateLock lock;
        if (phone != BLE_HS_CONN_HANDLE_NONE) failLinkLocked("Notificacao BLE falhou");
    }
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
        // getValue() retorna cópia NimBLEAttValue; nada referencia a stack após retorno.
        const auto value = characteristic->getValue();
        P::Frame frame;
        const P::Validation validation = P::decode(value.data(), value.size(), frame);
        StateLock lock;
        if (phone != info.getConnHandle()) return;
        if (!readyLocked() || !secure(info)) {
            failLinkLocked("RX BLE sem sessao segura pronta");
            return;
        }
        // Mesmo um frame velho malformado não pode derrubar a abertura atual.
        if (value.size() >= P::kHeaderSize && value.data()[2] < P::kChannels) {
            const uint8_t id = value.data()[2];
            const uint16_t epoch = static_cast<uint16_t>(value.data()[3] | (value.data()[4] << 8));
            if (epoch != channels[id].protocol.epoch()) return;
            if (validation != P::Validation::Ok) {
                abortChannelLocked(id, "Frame BLE invalido");
                return;
            }
        } else {
            failLinkLocked("Cabecalho BLE invalido");
            return;
        }
        LocalChannel& c = channels[frame.channel];
        // Pacotes de um canal já liberado não geram ERROR/replay.
        if (c.protocol.state() == P::State::Idle || c.protocol.state() == P::State::Aborted) return;
        const auto reception = c.protocol.receive(frame);
        if (reception == P::Reception::Fault) {
            abortChannelLocked(frame.channel, "Sequencia ou janela BLE invalida");
        } else if (reception == P::Reception::RemoteError) {
            wakeWriterLocked(c);
            setError("Ponte TLS retornou erro");
        } else if (reception == P::Reception::Accepted) {
            if (frame.type == P::Type::Ack) {
                c.lastProgress = millis();
                if (c.writerTask) xTaskNotifyGive(c.writerTask);
            }
            if (frame.type == P::Type::Data) drainEmptyLocked(frame.channel);
        }
    }
};
ServerCallbacks serverCallbacks;
CharacteristicCallbacks characteristicCallbacks;

// Chamado apenas com ioMutex. Não apaga objetos: callbacks podem estar ativos.
void stopRadio() {
    auto* advertising = NimBLEDevice::getAdvertising();
    const bool advertisingStopped = !advertising->isAdvertising() || advertising->stop();
    uint16_t handle;
    {
        StateLock lock;
        handle = phone;
    }
    if (handle != BLE_HS_CONN_HANDLE_NONE) server->disconnect(handle);
    {
        StateLock lock;
        radioStopPending = !advertisingStopped || phone != BLE_HS_CONN_HANDLE_NONE;
        if (!advertisingStopped && !errorText.load(std::memory_order_acquire)[0]) setError("Falha parando anuncios BLE");
    }
}

void worker(void*) {
    while (!workerExit.load(std::memory_order_acquire)) {
        bool work, stopping;
        {
            StateLock lock;
            const uint32_t now = millis();
            if (readyLocked()) {
                for (int i = 0; i < P::kChannels; ++i) {
                    if (channels[i].protocol.pending() &&
                        static_cast<uint32_t>(now - channels[i].lastProgress) >= kAckTimeoutMs)
                        abortChannelLocked(i, "Timeout de ACK BLE");
                }
            }
            stopping = radioStopPending;
            work = stopping || queueCount;
        }
        if (!work) {
            // ACK deadlines continuam sendo observados sem polling apertado.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
            continue;
        }
        if (xSemaphoreTake(ioMutex, pdMS_TO_TICKS(20)) != pdTRUE) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
            continue;
        }
        if (stopping) stopRadio();
        // Small bounded batch: drain queued work without a fixed sleep per GATT
        // frame, then yield so radio/lifecycle work on this core remains fair.
        for (unsigned sent = 0; sent < 8; ++sent) {
            Queued item;
            bool haveItem = false, send = false;
            uint16_t handle = BLE_HS_CONN_HANDLE_NONE;
            {
                StateLock lock;
                if (radioStopPending || !queueCount) break;
                item = queue[queueHead];
                queue[queueHead] = Queued{};
                queueHead = (queueHead + 1) % kQueueSize;
                --queueCount;
                haveItem = true;
                const auto& c = channels[item.frame.channel].protocol;
                send = readyLocked() && item.link == linkGeneration &&
                       item.generation == c.generation() && item.frame.epoch == c.epoch();
                handle = phone;
            }
            if (!haveItem) break;
            if (send) {
                uint8_t wire[P::kMaxFrame];
                const size_t length = P::encode(item.frame, wire, sizeof(wire));
                // NimBLE copies bytes into an mbuf before returning.
                if (!length || !tx->notify(wire, length, handle)) {
                    StateLock lock;
                    if (item.link == linkGeneration) failLinkLocked("Envio BLE falhou");
                }
            }
        }
        xSemaphoreGive(ioMutex);
        taskYIELD();
    }
    workerExited.store(true, std::memory_order_release);
    vTaskSuspend(nullptr); // lifecycle faz vTaskDelete depois do handshake
}

void stopState() {
    StateLock lock;
    enabled = false;
    authenticated = subscribed = false;
    disconnectPending = phone != BLE_HS_CONN_HANDLE_NONE;
    radioStopPending = initialized;
    ++linkGeneration;
    clearQueueLocked();
    for (auto& channel : channels) {
        wakeWriterLocked(channel);
        channel.protocol.release();
    }
    wakeWorkerLocked();
}

bool stopImpl(uint32_t timeout) {
    stopState();
    if (!initialized) return true;
    const uint32_t started = millis();
    if (xSemaphoreTake(ioMutex, pdMS_TO_TICKS(timeout)) != pdTRUE) {
        setError("Timeout parando radio BLE");
        return false;
    }
    stopRadio();
    xSemaphoreGive(ioMutex);
    do {
        {
            StateLock lock;
            if (phone == BLE_HS_CONN_HANDLE_NONE && !radioStopPending) return true;
        }
        if (expired(started, timeout)) break;
        pause();
    } while (true);
    setError("Timeout desconectando BLE");
    return false;
}
} // namespace

bool begin(uint32_t passkey) {
    Lifecycle lifecycle;
    if (!lifecycle.held) return false;
    if (finalShutdown) { setError("Shutdown BLE terminal"); return false; }
    if (passkey > 999999) { setError("PIN BLE invalido"); return false; }
    if (initializationFailed) { setError("Inicializacao BLE incompleta; requer shutdown"); return false; }
    if (!ioMutex) ioMutex = xSemaphoreCreateMutexStatic(&ioStorage);
    if (!ioMutex || xSemaphoreTake(ioMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        setError("Lifecycle BLE indisponivel"); return false;
    }
    bool ok = true;
    if (!initialized) {
        // Não assumir propriedade de uma stack já inicializada por outro módulo.
        if (NimBLEDevice::isInitialized()) {
            xSemaphoreGive(ioMutex);
            setError("Stack NimBLE ja pertence a outro modulo");
            return false;
        }
        if (!NimBLEDevice::init("IdeiaRec")) {
            initializationFailed = true;
            xSemaphoreGive(ioMutex);
            setError("Inicializacao NimBLE falhou");
            return false;
        }
        initialized = true;
        ok = NimBLEDevice::setCustomGapHandler(pairingDiagnostic);
        pairingPin.store(passkey, std::memory_order_release);
        NimBLEDevice::setSecurityPasskey(passkey);
        NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
        NimBLEDevice::setSecurityAuth(true, true, true);
        ble_hs_cfg.sm_sc_only = 1; // sm_sc sozinho permite fallback legado.
        ok = ok && NimBLEDevice::setMTU(P::kMinMtu);
        server = NimBLEDevice::createServer();
        if (server) {
            server->setCallbacks(&serverCallbacks, false); // callbacks estáticos não pertencem ao NimBLE
            server->advertiseOnDisconnect(false);
            auto* service = server->createService(kService);
            if (service) {
                auto* rx = service->createCharacteristic(kRx, NIMBLE_PROPERTY::WRITE |
                    NIMBLE_PROPERTY::WRITE_NR |
                    NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_AUTHEN, P::kMaxFrame);
                tx = service->createCharacteristic(kTx, NIMBLE_PROPERTY::NOTIFY |
                    NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::READ_AUTHEN, P::kMaxFrame);
                if (rx && tx) {
                    rx->setCallbacks(&characteristicCallbacks);
                    tx->setCallbacks(&characteristicCallbacks);
                    ok = ok && server->start();
                } else ok = false;
            } else ok = false;
        } else ok = false;
        auto* advertising = NimBLEDevice::getAdvertising();
        ok = ok && advertising && advertising->addServiceUUID(kService);
        if (advertising) {
            // 3 flags + 18 UUID128 + 10 nome = 31 bytes, sem scan response.
            ok = ok && advertising->setName("IdeiaRec");
            advertising->enableScanResponse(false);
        }
        if (ok) {
            workerExit.store(false, std::memory_order_release);
            workerExited.store(false, std::memory_order_release);
            ok = xTaskCreate(worker, "phone-relay", 4096, nullptr, 1, &workerHandle) == pdPASS;
        }
        if (!ok) initializationFailed = true;
    } else if (pairingPin.load(std::memory_order_acquire) != passkey) {
        // Rotação de PIN/bonds pertence ao portal/coordenador, nunca durante sessão.
        ok = false;
        setError("PIN BLE alterado; reinicie para reconfigurar");
    }
    if (ok) {
        bool advertise;
        {
            StateLock lock;
            if (disconnectPending || radioStopPending) ok = false;
            else enabled = true;
            advertise = ok && phone == BLE_HS_CONN_HANDLE_NONE;
        }
        if (advertise) ok = NimBLEDevice::startAdvertising();
        if (!ok) {
            stopState();
            setError("Nao foi possivel anunciar BLE");
        } else setError("");
    } else if (initializationFailed) setError("Configuracao NimBLE incompleta");
    xSemaphoreGive(ioMutex);
    return ok;
}

bool ready() { StateLock lock; return readyLocked(); }
bool active() { StateLock lock; return enabled; }
Diagnostics diagnostics() {
    StateLock lock;
    return {phone != BLE_HS_CONN_HANDLE_NONE, authenticated, subscribed, mtu,
            securityResult.load(), bondResult.load(), disconnectReason.load(), authenticationFlags.load(), passkeyRequests.load(),
            interval1250us.load(), latency.load(), supervisionTimeout10ms.load(), maxTxOctets.load(), maxRxOctets.load(),
            maxTxTime.load(), maxRxTime.load(), txPhy.load(), rxPhy.load()};
}
bool waitReady(uint32_t timeoutMs) {
    const uint32_t start = millis();
    const uint32_t timeout = bounded(timeoutMs);
    do {
        if (ready()) return true;
        if (!active()) return false;
        if (expired(start, timeout)) break;
        pause();
    } while (true);
    setError("Timeout aguardando telefone pronto");
    return false;
}
void stop() {
    Lifecycle lifecycle;
    if (lifecycle.held) stopImpl(2000);
}

int open(const char* host, uint16_t port, uint32_t timeoutMs) {
    P::Frame request;
    if (!P::makeOpen(0, 0, host, port, request)) { setError("Destino TLS invalido"); return -1; }
    int id = -1;
    uint32_t generation = 0;
    {
        StateLock lock;
        if (!readyLocked()) { setError("Telefone nao esta pronto"); return -1; }
        for (int i = 0; i < P::kChannels; ++i) {
            auto& c = channels[i].protocol;
            if (c.state() == P::State::Idle && c.start(static_cast<uint8_t>(i))) {
                id = i;
                generation = c.generation();
                request.channel = static_cast<uint8_t>(i);
                request.epoch = c.epoch();
                if (!enqueueLocked(request)) {
                    c.release();
                    return -1;
                }
                break;
            }
        }
    }
    if (id < 0) { setError("Quatro canais TLS ocupados"); return -1; }
    const uint32_t start = millis();
    const uint32_t timeout = bounded(timeoutMs);
    do {
        {
            StateLock lock;
            auto& c = channels[id].protocol;
            if (c.generation() != generation || !readyLocked()) {
                // Falha invalida token. O slot ainda é reservado a esta abertura;
                // não liberar um canal que já foi reutilizado por outro chamador.
                if (c.state() == P::State::Aborted && c.epoch() == request.epoch) c.release();
                return -1;
            }
            if (c.state() == P::State::Open) return id;
            if (c.state() != P::State::Opening || expired(start, timeout)) {
                setError(c.state() == P::State::PeerClosed ? "TLS fechou durante OPEN" : "Timeout abrindo TLS");
                c.release();
                enqueueLocked(controlLocked(id, P::Type::Close));
                return -1;
            }
        }
        pause();
    } while (true);
}

size_t write(int channel, const uint8_t* data, size_t size, uint32_t timeoutMs) {
    if (!validChannel(channel) || (!data && size)) { setError("Escrita TLS invalida"); return 0; }
    if (!size) return 0;
    uint32_t generation;
    {
        StateLock lock;
        auto& c = channels[channel];
        if (!readyLocked() || c.protocol.state() != P::State::Open) { setError("Canal TLS fechado"); return 0; }
        if (c.writing) { setError("Escrita concorrente no canal TLS"); return 0; }
        c.writing = true;
        c.writerTask = xTaskGetCurrentTaskHandle();
        generation = c.protocol.generation();
    }
    size_t written = 0;
    const uint32_t start = millis();
    const uint32_t timeout = bounded(timeoutMs);
    do {
        {
            StateLock lock;
            auto& c = channels[channel];
            if (c.protocol.generation() != generation) return written;
            if (!readyLocked() || c.protocol.state() != P::State::Open) {
                abortChannelLocked(channel, "Canal TLS interrompido");
                return written;
            }
            while (c.protocol.credits() && written < size) {
                P::Frame frame;
                const size_t rest = size - written;
                const size_t count = rest < P::kMaxPayload ? rest : P::kMaxPayload;
                const bool wasEmpty = c.protocol.pending() == 0;
                if (!c.protocol.prepareData(data + written, count, frame) || !enqueueLocked(frame)) {
                    abortChannelLocked(channel, "Fila de escrita TLS falhou");
                    return written;
                }
                if (wasEmpty) c.lastProgress = millis();
                written += count;
            }
            if (written == size) {
                c.writing = false;
                c.writerTask = nullptr;
                return written;
            }
            if (expired(start, timeout)) {
                abortChannelLocked(channel, "Timeout de credito TLS");
                return written;
            }
        }
        const uint32_t elapsed = static_cast<uint32_t>(millis() - start);
        const uint32_t remaining = elapsed >= timeout ? 0 : timeout - elapsed;
        TickType_t ticks = pdMS_TO_TICKS(remaining);
        if (!ticks && remaining) ticks = 1;
        ulTaskNotifyTake(pdTRUE, ticks); // ACK/abort wakes this writer; no 1 ms credit polling.
    } while (true);
}

int read(int channel, uint8_t* data, size_t size) {
    if (!validChannel(channel) || (!data && size)) { setError("Leitura TLS invalida"); return -1; }
    if (!size) return 0;
    StateLock lock;
    auto& c = channels[channel].protocol;
    if (!readyLocked()) return -1;
    if (c.state() != P::State::Open && c.state() != P::State::PeerClosed) return -1;
    drainEmptyLocked(channel);
    const P::Consumption consumed = c.consume(data, size);
    if (consumed.ack && !ackLocked(channel, consumed.sequence)) return -1;
    drainEmptyLocked(channel);
    return consumed.bytes ? static_cast<int>(consumed.bytes) : -1;
}
int available(int channel) {
    if (!validChannel(channel)) return 0;
    StateLock lock;
    return readyLocked() ? static_cast<int>(channels[channel].protocol.available()) : 0;
}
int peek(int channel) {
    if (!validChannel(channel)) return -1;
    StateLock lock;
    return readyLocked() ? channels[channel].protocol.peek() : -1;
}
bool connected(int channel) {
    if (!validChannel(channel)) return false;
    StateLock lock;
    const auto& c = channels[channel].protocol;
    return readyLocked() && (c.state() == P::State::Open ||
           (c.state() == P::State::PeerClosed && c.available() != 0));
}
void close(int channel) {
    if (!validChannel(channel)) return;
    StateLock lock;
    auto& c = channels[channel];
    if (c.protocol.state() == P::State::Idle) return;
    c.protocol.release();
    wakeWriterLocked(c);
    if (readyLocked()) enqueueLocked(controlLocked(channel, P::Type::Close));
}
const char* lastError() { return errorText.load(std::memory_order_acquire); }

bool shutdown(uint32_t timeoutMs) {
    Lifecycle lifecycle;
    if (!lifecycle.held) return false;
    finalShutdown = true; // também após timeout: somente retry de shutdown é seguro
    const uint32_t start = millis();
    const uint32_t timeout = bounded(timeoutMs);
    if (!stopImpl(timeout)) return false;
    if (workerHandle) {
        workerExit.store(true, std::memory_order_release);
        while (!workerExited.load(std::memory_order_acquire)) {
            if (expired(start, timeout)) { setError("Timeout encerrando worker BLE"); return false; }
            pause();
        }
        vTaskDelete(workerHandle);
        workerHandle = nullptr;
    }
    if (initialized || initializationFailed) {
        const uint32_t elapsed = static_cast<uint32_t>(millis() - start);
        const uint32_t remaining = elapsed < timeout ? timeout - elapsed : 0;
        if (xSemaphoreTake(ioMutex, pdMS_TO_TICKS(remaining)) != pdTRUE) {
            setError("Timeout liberando NimBLE"); return false;
        }
        // Upstream clearAll=true apaga objetos MESMO se stop falhar. Primeiro
        // confirma parada, só então apaga objetos e invalida nossos ponteiros.
        if (!NimBLEDevice::deinit(false)) {
            xSemaphoreGive(ioMutex);
            setError("NimBLE recusou shutdown"); return false;
        }
        NimBLEDevice::deinit(true);
        server = nullptr;
        tx = nullptr;
        initialized = initializationFailed = false;
        pairingPin.store(0, std::memory_order_release);
        xSemaphoreGive(ioMutex);
    }
    finalShutdown = true;
    return true;
}

} // namespace PhoneRelay

#endif // ARDUINO_ARCH_ESP32
