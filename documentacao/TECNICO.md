# DMK Smart 1.1 — documentação técnica

## Estrutura

```
DMK.bat                     menu (Windows) → chama scripts/dmk.py
platformio.ini              6 ambientes: SERVE/CONTROLE × ESP32/ESP32-S3/ESP8266
include/
  DmkVersion.h              versões independentes (SERVE, CONTROLE)
  DmkConfig.h               repositório GitHub padrão, tempos de enlace e pareamento
  DmkProto.h                formato dos quadros ESP-NOW/UDP
  DmkLink.h                 enlace (ESP-NOW + UDP + pareamento)
  DmkCrypto.h               SHA-256, HMAC, X25519 portáteis
  DmkStatusLed.h            padrões do LED azul
  RemoteOta.h / MqttOta.h   OTA (GitHub/servidor) e canal de manutenção MQTT
  DmkRootCerts.h            certificados raiz (gerado por scripts/gerar_certificados.py)
  serve/ controle/          pinos, configurações e DmkWebAssets.h (gerado)
src/common/                 código compartilhado
src/serve/                  firmware SERVE (bomba) — lógica da 1.0.0 + enlace
src/controle/main.cpp       firmware CONTROLE — lógica da 1.0.0 + enlace
data/serve, data/controle   fontes do painel web (embutidas no firmware ao compilar)
app/                        APPDMK-UNIFICADO.html (app MQTT original) e ATUALIZAR-DMK.html
scripts/                    pio_pre.py, pio_post.py, embed_web.py, dmk.py, gerar_certificados.py
.github/workflows/release.yml  compila e publica a Release ao receber a tag dmk-*
test/                       testes no PC (run_tests.sh)
```

## Prioridade de comunicação (CONTROLE → SERVE)

1. **ESP-NOW** unicast para o MAC do SERVE pareado. Sem ACK em 350 ms →
2. **UDP** para o IP do SERVE (porta 47800). Sem ACK em 700 ms →
3. **MQTT** `<base>/pump/set` ou `<base>/light/set` com `ON`/`OFF` (igual à 1.0.0).

O SERVE envia `STATUS` por ESP-NOW (broadcast, 2 s) e UDP (broadcast, 3 s), e na hora
em que algo muda. O CONTROLE considera um caminho "ativo" se recebeu STATUS por ele
nos últimos 7 s. Os comandos são absolutos (ON/OFF), então repetir por outro caminho
não inverte nada. No SERVE, comandos de ESP-NOW/UDP entram na mesma fila do MQTT
(`PumpHistory::queueCommand`) e passam pela proteção de tensão e pelo histórico.

## Quadro autenticado

```
Header 22 B: 'D' 'K' ver tipo | net(4) | boot(4) | seq(4) | mac_origem(6)
Payload    : cifrado (fluxo HMAC-SHA256(encKey, "P"|mac|boot|seq|i))
Tag 8 B    : HMAC-SHA256(macKey, header|payload_cifrado)[0..8]
```
`encKey`, `macKey` e `net` derivam da chave de 16 bytes do sistema (SHA-256 com rótulos).
Anti-repetição: janela deslizante de 64 por remetente sobre (boot, seq); `boot` é
salvo na memória e incrementa a cada reinício. `CMD` leva ainda a sessão do SERVE
(32 bits aleatórios, troca a cada boot e a cada 10 min; aceita atual e anterior).

## Pareamento

1. CONTROLE envia `PAIR_REQ` (broadcast, varrendo canais 1–13) com sua chave pública X25519.
2. SERVE (janela aberta) responde `PAIR_RESP` com sua chave pública e, cifrados com
   `K = SHA256("DMK-PAIR-v1"|X25519|pubC|pubS)`: chave do sistema, MAC, IP, nome, canal.
   Em seguida `PAIR_CFG` (até 4 pedaços) com Wi-Fi/MQTT, se compartilhamento ligado.
3. CONTROLE grava a chave e envia `PAIR_DONE` já autenticado. O SERVE registra o controle.

Um observador passivo não obtém a chave. Um atacante ativo precisaria estar presente
durante a janela (2 min, aberta pelo botão físico ou na 1ª instalação).

## OTA

`manifest.json` da Release:
```json
{"release":"dmk-s1.1.1-c1.1.0","targets":{"SERVE_ESP32":{"version":"1.1.1","file":"DMK-SERVE_ESP32-1.1.1.bin",
 "size":1410755,"sha256":"…","url":"https://github.com/<repo>/releases/download/<tag>/DMK-SERVE_ESP32-1.1.1.bin"}}}
```
A placa usa `https://github.com/<repo>/releases/latest/download/manifest.json`, pega só a
entrada do seu alvo (`DMK_OTA_TARGET`), exige mesma origem do manifesto, baixa em etapas
sem travar o loop, confere SHA-256 antes de `Update.end()` e recusa versão menor.
Formato antigo de manifesto (um alvo, campo `device`) continua aceito.

Canal MQTT de manutenção: `dmk-ota/<id>/status` (retido) e `dmk-ota/<id>/cmd`.
Comando: `{"id","cmd","ts","p","sig"}` com `sig = HMAC(senha, id\ncmd\nts\np)`.

## Rotas HTTP novas

| Placa | Rota | Uso |
|---|---|---|
| SERVE | `GET /api/link` | canal, MAC, controles pareados, janela |
| SERVE | `POST /api/link/pair` `state=ON/OFF` | abre/fecha pareamento |
| SERVE | `POST /api/link/remove` `mac=` | remove controle da lista |
| SERVE | `POST /api/link/rekey` `confirm=TROCAR` | troca a chave (desfaz pareamentos) |
| SERVE | `POST /api/link/share` `enabled=1/0` | compartilhar Wi-Fi/MQTT no pareamento |
| SERVE | `POST /api/ota/remote/config` `repo,manifestUrl,autoInstall,localHttp` | origem das atualizações |
| CONTROLE | `POST /api/pair` `state=ON/OFF` · `POST /api/pair/forget` | pareamento |
| ambas | `GET /ota-mqtt` | página de atualizações |

Rotas que alteram estado exigem Basic Auth `admin:<senha do painel>`, exceto pelo AP de configuração.

## Compatibilidade testada

Compila sem erros com Arduino-ESP32 2.0.17 (IDF 4.4) e 3.3.6 (IDF 5.5) e ESP8266 3.1.2.
`test/run_tests.sh`: vetores oficiais (SHA-256, HMAC RFC 4231, X25519 RFC 7748),
janela anti-repetição e simulação SERVE↔CONTROLE com o `DmkLink.cpp` real (pareamento
com varredura de canal, sigilo, replay, adulteração, UDP, troca de chave).

## Atualizando a partir da 1.0.0

As configurações (Wi-Fi, MQTT, calibrações, automações) usam as mesmas chaves e são
mantidas. A 1.0.0 usava outro sistema de OTA, então a primeira passagem para 1.1 é pela
USB (DMK.bat) ou pelo envio do `.bin` no painel local. A nova tabela de partições do
ESP32 (apps de 1856 KB) só é aplicada em instalação limpa pela USB.
