#pragma once
#include <stdint.h>

#if ARDUINO_USB_CDC_ON_BOOT
class CaptureSession;
class Recorder;
class AudioCodec;

// Diagnostico sincronico USB: caller exige storageReady/canSleep e ownership
// exclusivo dos objetos do App durante a chamada. Nao altera Settings/NVS/sync.
// captured_samples conta PCM de SAIDA persistido, nao amostras ADC de ingresso.
// Timeout de 10s pede parada e retorna false SEM liberar recursos ainda usados
// por workers. Os objetos devem continuar vivos; nao dormir/reusar ate o join.
// Nao mede wake real, audio analogicamente valido, corrente ou isolamento fisico.
bool runPowerSelfTest(CaptureSession &session, Recorder &recorder, AudioCodec &codec,
                      float micGainDb, uint32_t repetitions = 1);
#endif
