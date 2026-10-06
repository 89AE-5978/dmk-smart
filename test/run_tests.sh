#!/bin/sh
# Testes no PC (Linux/macOS/WSL, precisa g++): criptografia, anti-repeticao e
# simulacao SERVE <-> CONTROLE com o DmkLink.cpp real sobre radio simulado.
set -e
cd "$(dirname "$0")"
INC="-I. -Imock -I../include"
g++ -std=c++17 -O2 -I../include test_crypto.cpp ../src/common/DmkCrypto.cpp -o /tmp/dmk_test_crypto && /tmp/dmk_test_crypto
g++ -std=c++17 test_replay.cpp -o /tmp/dmk_test_replay && /tmp/dmk_test_replay
g++ -std=c++17 -O1 -w $INC -c ../src/common/DmkCrypto.cpp -o /tmp/dmk_c.o
g++ -std=c++17 -O1 -w $INC -DDMK_ROLE_SERVE -DDmkLink=DmkLinkS -c ../src/common/DmkLink.cpp -o /tmp/dmk_ls.o
g++ -std=c++17 -O1 -w $INC -DDMK_ROLE_CONTROLE -DDmkLink=DmkLinkC -c ../src/common/DmkLink.cpp -o /tmp/dmk_lc.o
g++ -std=c++17 -O1 -w $INC -DDMK_ROLE_SERVE -DDmkLink=DmkLinkS -DSIDE_PREFIX=s_ -c sim_wrap.cpp -o /tmp/dmk_ws.o
g++ -std=c++17 -O1 -w $INC -DDMK_ROLE_CONTROLE -DDmkLink=DmkLinkC -DSIDE_PREFIX=c_ -c sim_wrap.cpp -o /tmp/dmk_wc.o
g++ -std=c++17 -O1 -w $INC -DDMK_ROLE_SERVE -DDmkLink=DmkLinkS -DSIDE_PREFIX=s_ -c sim_glue.cpp -o /tmp/dmk_gs.o
g++ -std=c++17 -O1 -w $INC -DDMK_ROLE_CONTROLE -DDmkLink=DmkLinkC -DSIDE_PREFIX=c_ -c sim_glue.cpp -o /tmp/dmk_gc.o
g++ -std=c++17 -O1 -w $INC -c sim_mock.cpp -o /tmp/dmk_m.o
g++ -std=c++17 -O1 -w $INC -c sim_main.cpp -o /tmp/dmk_main.o
g++ /tmp/dmk_c.o /tmp/dmk_ls.o /tmp/dmk_lc.o /tmp/dmk_ws.o /tmp/dmk_wc.o /tmp/dmk_gs.o /tmp/dmk_gc.o /tmp/dmk_m.o /tmp/dmk_main.o -o /tmp/dmk_sim
/tmp/dmk_sim | grep -E "^(OK|FAIL)|TODOS|FALHA"
