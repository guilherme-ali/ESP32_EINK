#pragma once
#include <Arduino.h>
#include "../board/rtc.h"

// Gerencia as gravacoes em /notes/ no LittleFS: listar, gerar o proximo
// nome de arquivo (carimbado com hora do RTC) e reportar espaco livre.
// Cada nota e por enquanto so um .wav (Fase 5 acrescenta o .txt da
// transcricao, Fase 6 marca upload).
struct NoteEntry {
  char path[48]; // "/notes/YYYYMMDD-HHMMSS.wav"
  char label[24]; // "YYYYMMDD-HHMMSS" sem extensao, para exibir na tela
  uint32_t sizeBytes;
  uint32_t sampleRateHz; // lido do cabecalho WAV - notas antigas podem
                          // ter sido gravadas numa taxa diferente da atual
};

class NotesStore {
public:
  bool begin();

  // Usa RtcDateTime ja lido (ver board/rtc.h) para nomear o arquivo.
  void buildPath(const RtcDateTime &now, char *outPath, size_t outLen);

  int count();
  bool getAt(int index, NoteEntry &out); // 0 = mais recente

  // Quantos .wav em /notes NAO tem um NOME.snc do lado (ainda nao
  // subiram pro Drive). O marcador .snc e criado pelo cliente do Drive
  // depois de um upload bem-sucedido.
  int countPendingSync();

  uint64_t totalBytes();
  uint64_t usedBytes();
  uint64_t freeBytes();
};
