#pragma once
#include <Arduino.h>
// Canal de manutencao MQTT separado (topico dmk-ota/<id>/...), usado pelo
// painel "Atualizacoes". Nao interfere nos topicos operacionais.
namespace MqttOta {
void begin();
void loop(const String &host, uint16_t port, const String &user, const String &password, const String &otaPassword);
String statusJson();
bool request(const String &json, String &error);       // MQTT: exige assinatura
bool requestLocal(const String &json, String &error);  // HTTP local ja autenticado
bool busy();
String deviceId();
} // namespace MqttOta

// Ganchos implementados no main de cada placa.
bool dmkPrepareMqttOta(String &error);  // coloca saidas em estado seguro
void dmkEndMqttOta(bool success);
bool dmkCanAutoUpdate();                // ex.: SERVE so atualiza sozinho com a bomba desligada
