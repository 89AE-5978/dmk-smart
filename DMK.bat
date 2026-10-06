@echo off
setlocal EnableExtensions
chcp 65001 >nul
title DMK SMART - Central de instalacao e atualizacao
cd /d "%~dp0"

rem ---------------------------------------------------------------- cores
for /F %%a in ('echo prompt $E ^| cmd') do set "ESC=%%a"
set "AZ=%ESC%[96m"
set "VD=%ESC%[92m"
set "AM=%ESC%[93m"
set "VM=%ESC%[91m"
set "NG=%ESC%[1m"
set "RS=%ESC%[0m"

rem ---------------------------------------------------------------- python
set "PY="
where py >nul 2>nul && set "PY=py -3"
if not defined PY where python >nul 2>nul && set "PY=python"
if not defined PY goto :sem_python
%PY% -c "import sys; sys.exit(0 if sys.version_info>=(3,9) else 1)" >nul 2>nul || goto :sem_python

:menu
cls
echo %AZ%==================================================================%RS%
echo %NG%   DMK SOLUCOES SMART  -  SERVE (bomba)  +  CONTROLE (botoes)%RS%
echo %AZ%==================================================================%RS%
%PY% scripts\dmk.py status
echo.
echo   %NG%INSTALAR%RS%
echo     [1] Preparar ferramentas  %AM%(primeira vez - instala o PlatformIO)%RS%
echo     [2] Gravar SERVE pela USB
echo     [3] Gravar CONTROLE pela USB
echo     [4] Monitor serial  %AM%(ver o que a placa esta fazendo)%RS%
echo.
echo   %NG%ATUALIZAR PELO GITHUB%RS%
echo     [5] Configurar GitHub  %AM%(usuario/repositorio - uma vez)%RS%
echo     [6] Nova versao  %AM%(SERVE, CONTROLE ou ambos)%RS%
echo     [7] Publicar no GitHub  %AM%(as placas atualizam pela internet)%RS%
echo     [8] Abrir painel de atualizacoes
echo.
echo   %NG%OUTROS%RS%
echo     [9] Compilar as 6 placas  %AM%(sem gravar)%RS%
echo     [P] Listar portas USB      [L] Ler LEIA-PRIMEIRO      [0] Sair
echo.
choice /c 123456789PL0 /n /m "   Escolha uma opcao: "
set "OP=%errorlevel%"
if "%OP%"=="1" goto :preparar
if "%OP%"=="2" (set "ROLE=SERVE" & goto :gravar)
if "%OP%"=="3" (set "ROLE=CONTROLE" & goto :gravar)
if "%OP%"=="4" goto :monitor
if "%OP%"=="5" goto :github
if "%OP%"=="6" goto :versao
if "%OP%"=="7" goto :publicar
if "%OP%"=="8" goto :painel
if "%OP%"=="9" goto :compilar
if "%OP%"=="10" goto :portas
if "%OP%"=="11" goto :leia
goto :fim

rem ======================================================================
:preparar
cls
echo %NG%Preparando ferramentas...%RS%
%PY% scripts\dmk.py preparar
goto :pausa

:compilar
cls
%PY% scripts\dmk.py compilar
goto :pausa

:portas
cls
echo %NG%Portas encontradas:%RS%
%PY% scripts\dmk.py portas
goto :pausa

rem ---------------------------------------------------------------- gravar
:gravar
cls
echo %NG%Gravar %ROLE% pela USB%RS%
echo.
echo   Qual placa?
echo     [1] ESP32 comum (DevKit 4 MB)  %AM%- placa atual%RS%
echo     [2] ESP32-S3 (8 MB)
echo     [3] ESP8266 / NodeMCU / ESP-12E
echo     [0] Voltar
choice /c 1230 /n /m "   Placa: "
if errorlevel 4 goto :menu
if errorlevel 3 (set "BOARD=ESP8266" & goto :gravar_porta)
if errorlevel 2 (set "BOARD=ESP32S3" & goto :gravar_porta)
set "BOARD=ESP32"

:gravar_porta
echo.
echo %NG%Portas encontradas:%RS%
%PY% scripts\dmk.py portas
echo.
set "PORTA="
set /p "PORTA=   Digite a porta (ex.: COM5) e ENTER: "
if not defined PORTA goto :menu
echo.
echo   %AM%Instalacao limpa apaga Wi-Fi, pareamento e configuracoes da placa.%RS%
choice /c SN /n /m "   Fazer instalacao limpa? [S/N] "
set "APAGAR="
if errorlevel 2 goto :gravar_go
set "APAGAR=--apagar"

:gravar_go
echo.
%PY% scripts\dmk.py gravar %ROLE%_%BOARD% %PORTA% %APAGAR%
echo.
choice /c SN /n /m "   Abrir o monitor serial agora? [S/N] "
if errorlevel 2 goto :pausa
%PY% scripts\dmk.py monitor %PORTA%
goto :pausa

rem --------------------------------------------------------------- monitor
:monitor
cls
%PY% scripts\dmk.py portas
echo.
set "PORTA="
set /p "PORTA=   Porta (ex.: COM5): "
if not defined PORTA goto :menu
%PY% scripts\dmk.py monitor %PORTA%
goto :pausa

rem ---------------------------------------------------------------- github
:github
cls
echo %NG%Configurar GitHub%RS%
echo.
echo   1. Crie uma conta em https://github.com (gratis).
echo   2. Crie um repositorio PUBLICO vazio, ex.: dmk-smart
echo   3. Digite abaixo no formato  usuario/repositorio
echo.
set "REPO="
set /p "REPO=   Repositorio: "
if not defined REPO goto :menu
%PY% scripts\dmk.py github %REPO%
goto :pausa

rem ---------------------------------------------------------------- versao
:versao
cls
echo %NG%Nova versao%RS%
echo     [1] So o SERVE
echo     [2] So o CONTROLE
echo     [3] Os dois
echo     [0] Voltar
choice /c 1230 /n /m "   Qual? "
if errorlevel 4 goto :menu
if errorlevel 3 (set "QUAL=AMBOS" & goto :versao_tipo)
if errorlevel 2 (set "QUAL=CONTROLE" & goto :versao_tipo)
set "QUAL=SERVE"
:versao_tipo
echo.
echo     [1] Correcao      (1.1.0 -^> 1.1.1)
echo     [2] Novidade      (1.1.0 -^> 1.2.0)
echo     [3] Grande mudanca (1.1.0 -^> 2.0.0)
choice /c 123 /n /m "   Tipo: "
set "TIPO=patch"
if errorlevel 2 set "TIPO=minor"
if errorlevel 3 set "TIPO=major"
%PY% scripts\dmk.py versao %QUAL% %TIPO%
goto :pausa

rem -------------------------------------------------------------- publicar
:publicar
cls
echo %NG%Publicar nova versao no GitHub%RS%
echo.
echo     [1] GitHub compila e publica  %VD%(recomendado - so precisa do Git)%RS%
echo     [2] Compilar aqui e publicar  %AM%(precisa do GitHub CLI "gh")%RS%
echo     [0] Voltar
choice /c 120 /n /m "   Como? "
if errorlevel 3 goto :menu
if errorlevel 2 (%PY% scripts\dmk.py publicar-local & goto :pausa)
%PY% scripts\dmk.py publicar
goto :pausa

rem ---------------------------------------------------------------- painel
:painel
start "" "%~dp0app\ATUALIZAR-DMK.html"
goto :menu

:leia
start "" "%~dp0LEIA-PRIMEIRO.html"
goto :menu

rem ======================================================================
:sem_python
cls
echo %VM%Python 3.9 ou mais novo nao foi encontrado.%RS%
echo.
echo   1. Baixe em https://www.python.org/downloads/
echo   2. Na instalacao marque "Add python.exe to PATH".
echo   3. Feche e abra o DMK.bat de novo.
echo.
start "" "https://www.python.org/downloads/"
pause
exit /b 1

:pausa
echo.
pause
goto :menu

:fim
endlocal
exit /b 0
