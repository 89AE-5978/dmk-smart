#pragma once
// ============================================================================
// DMK SMART - configuracoes comuns as duas placas
// ============================================================================

// Repositorio GitHub onde as versoes (Releases) sao publicadas.
// Quando o firmware e compilado pelo GitHub Actions, este valor e preenchido
// automaticamente com o repositorio do proprio projeto. Para compilar no PC,
// troque abaixo OU configure depois pelo painel (Atualizacoes > Repositorio).
#ifndef DMK_GITHUB_REPO
#define DMK_GITHUB_REPO "SEU-USUARIO/dmk-smart"
#endif

// Verificacao automatica de nova versao (o botao "Verificar" e imediato).
#define DMK_OTA_CHECK_INTERVAL_MS (6UL * 3600UL * 1000UL)

// Pareamento ESP-NOW
#define DMK_PAIR_WINDOW_S 120          // janela aberta no SERVE (botao/painel)
#define DMK_PAIR_FIRST_BOOT_S 180      // janela automatica quando nao ha nada pareado
#define DMK_PAIR_HOLD_MS 3000UL        // tempo segurando o(s) botao(oes) para parear

// Enlace
#define DMK_STATUS_ESPNOW_MS 2000UL    // SERVE envia estado por ESP-NOW
#define DMK_STATUS_LAN_MS 3000UL       // SERVE envia estado por UDP
#define DMK_LINK_FRESH_MS 7000UL       // CONTROLE considera o caminho ativo
#define DMK_SESSION_ROTATE_MS 600000UL // sessao anti-repeticao do SERVE

// Seguranca do painel web: rotas que alteram algo exigem usuario "admin" e a
// senha do painel (a mesma senha OTA). Pelo AP de configuracao nao pede senha,
// pois para abrir o AP e preciso acesso fisico ao botao.
#define DMK_WEB_USER "admin"
