# DMK Smart — SERVE + CONTROLE

Monitoramento e controle de bomba de poço (ESP32 · ESP32-S3 · ESP8266).

- **SERVE**: mede tensões, corrente, fluxo, temperatura, chuva e alarme; aciona bomba e luz; automações e histórico.
- **CONTROLE**: botões BOMBA/LUZ/MENU, LCD e LEDs. Fala com o SERVE por **ESP-NOW → rede local → MQTT**.

## Começar
1. Windows: dois cliques em **`DMK.bat`** → *Preparar ferramentas* → *Gravar SERVE* / *Gravar CONTROLE*.
2. Abra **`LEIA-PRIMEIRO.html`** para o passo a passo, LED de status e pareamento.

## Atualizações
`DMK.bat` → *Nova versão* → *Publicar no GitHub*. O GitHub Actions compila as 6 placas e publica a Release
com `manifest.json`; as placas atualizam pelo painel **Atualizações** (local ou via MQTT).

Documentação técnica: [`documentacao/TECNICO.md`](documentacao/TECNICO.md) · Testes: `test/run_tests.sh`
