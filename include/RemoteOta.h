#pragma once
// ============================================================================
// ATUALIZACAO REMOTA (OTA) - GitHub Releases ou servidor proprio
//
// Padrao: https://github.com/<usuario>/<repositorio>/releases/latest/download/manifest.json
// O manifesto lista uma entrada por placa (SERVE_ESP32, CONTROLE_ESP8266...).
// Seguranca: HTTPS com certificados raiz embutidos, alvo da placa conferido,
// SHA-256 do arquivo conferido antes de ativar, nunca instala versao menor.
// ============================================================================
#include <Arduino.h>

namespace RemoteOta {

struct Manifest {
  String device;
  String version;
  String url;
  String sha256;
  String notes;
  size_t size = 0;
};

bool begin(const char *target, const char *installedVersion);

// repo = "usuario/repositorio" (GitHub). manifestUrl opcional substitui o GitHub.
bool saveConfig(const String &repo, const String &manifestUrl, const String &token,
                const String &caCert, String &error);
bool setAutoInstall(bool enabled, String &error);
bool autoInstall();
bool allowHttpLocal(bool enabled, String &error);
String configJson();
String effectiveManifestUrl();
bool configured();

bool check(Manifest &manifest, String &error);
int compareWithInstalled(const String &remoteVersion);

// Instalacao em etapas (nao bloqueia o loop): startInstall() e depois tick().
bool startInstall(bool force, String &error);
void tick();
bool busy();
bool finished();
String statusJson();

// Compatibilidade: instala de forma bloqueante.
bool installLatest(bool force, String &installedRemoteVersion, size_t &writtenBytes, String &error);

} // namespace RemoteOta
