#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>

// =============================================================================
//  ESTRUTURAS DE MENSAGEM
// =============================================================================
typedef struct { BYTE tipo; }                                MSG_CMD;
typedef struct { BYTE tipo; TCHAR msg[140]; DWORD duracao; } MSG_ALERTA;
typedef struct { BYTE tipo; DWORD identificador; }           MSG_ID;

typedef union {
    BYTE       raw[sizeof(MSG_ALERTA)];
    MSG_CMD    cmd;
    MSG_ALERTA alerta;
    MSG_ID     id;
} MSG_BUF;

// =============================================================================
//  GLOBAIS
// =============================================================================
HANDLE           hPipe = INVALID_HANDLE_VALUE;
CRITICAL_SECTION csCons;
volatile BOOL    ligado = FALSE;
volatile BOOL    bSair = FALSE;
DWORD            idProprio = 0;
TCHAR            g_nomePipe[256] = _T("");

// Evento manual-reset: sinalizado quando ficamos ligados ao central
HANDLE hEvtLigado = NULL;

// =============================================================================
//  Printf thread-safe
// =============================================================================
void PrintMsg(const TCHAR* m) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&csCons);
    _tprintf(_T("\n%02d/%02d/%d (%02d:%02d:%02d): %s\nCMD> "),
        st.wDay, st.wMonth, st.wYear,
        st.wHour, st.wMinute, st.wSecond, m);
    LeaveCriticalSection(&csCons);
}

// =============================================================================
//  Funcao auxiliar: WriteFile com Overlapped I/O
//  Necessario porque o pipe foi aberto com FILE_FLAG_OVERLAPPED.
//  Segundo a MSDN, usar WriteFile com NULL lpOverlapped num handle aberto
//  com FILE_FLAG_OVERLAPPED e comportamento indefinido.
// =============================================================================
static BOOL WriteOverlapped(HANDLE hP, const void* buf, DWORD sz) {
    HANDLE hEvt = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!hEvt) return FALSE;

    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = hEvt;

    DWORD written = 0;
    BOOL ok = WriteFile(hP, buf, sz, &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(hEvt, INFINITE);
        ok = GetOverlappedResult(hP, &ov, &written, FALSE);
    }
    CloseHandle(hEvt);
    return ok && (written == sz);
}

// =============================================================================
//  Funcao auxiliar: ReadFile com Overlapped I/O
//  Aguarda apenas ate maxMs milissegundos (usa INFINITE se maxMs == 0).
//  Devolve numero de bytes lidos, ou 0 em caso de erro/timeout.
// =============================================================================
static DWORD ReadOverlapped(HANDLE hP, void* buf, DWORD sz,
    HANDLE hExtra1, HANDLE hExtra2,
    DWORD* pSignal)
{
    // hExtra1 / hExtra2: handles adicionais para WaitForMultipleObjects
    // pSignal: indice do handle que acordou (0 = leitura, 1 = extra1, 2 = extra2)
    HANDLE hEvt = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!hEvt) return 0;

    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = hEvt;

    DWORD lidos = 0;
    BOOL okRead = ReadFile(hP, buf, sz, &lidos, &ov);
    DWORD errRead = GetLastError();

    if (okRead) {
        // Completou imediatamente (dados ja estavam no buffer)
        CloseHandle(hEvt);
        if (pSignal) *pSignal = 0;
        return lidos;
    }
    if (errRead != ERROR_IO_PENDING) {
        // Erro real (pipe partido, etc...)
        CloseHandle(hEvt);
        if (pSignal) *pSignal = 0;
        return 0;
    }

    // Operacao pendente - monta array de handles para WaitForMultipleObjects
    HANDLE waitH[3];
    DWORD  nWait = 0;
    waitH[nWait++] = hEvt;
    if (hExtra1 && hExtra1 != INVALID_HANDLE_VALUE) waitH[nWait++] = hExtra1;
    if (hExtra2 && hExtra2 != INVALID_HANDLE_VALUE) waitH[nWait++] = hExtra2;

    DWORD wr = WaitForMultipleObjects(nWait, waitH, FALSE, INFINITE);

    if (pSignal) *pSignal = (wr >= WAIT_OBJECT_0) ? (wr - WAIT_OBJECT_0) : 0xFF;

    if (wr == WAIT_OBJECT_0) {
        // Leitura concluida
        GetOverlappedResult(hP, &ov, &lidos, FALSE);
        CloseHandle(hEvt);
        return lidos;
    }

    // Outro handle acordou (timer ou evento de sair): cancela leitura pendente
    CancelIo(hP);
    GetOverlappedResult(hP, &ov, &lidos, TRUE); // aguarda cancelamento
    CloseHandle(hEvt);
    return 0; // 0 = leitura nao completou
}

// =============================================================================
//  Thread de alertas
//
// =============================================================================
DWORD WINAPI ThreadAlertas(LPVOID lpParam) {
    (void)lpParam;
    // Waitable timer de auto-reset para gerir a duracao do alerta.
    // FALSE = auto-reset: o timer volta ao estado nao-sinalizado automaticamente
    // depois de disparar uma vez. Com TRUE (reset manual) o timer ficaria
    // sinalizado para sempre e o WaitForMultipleObjects entraria em loop infinito.
    HANDLE hTimer = CreateWaitableTimer(NULL, FALSE, NULL);
    if (!hTimer) return 1;

    while (!bSair) {

        // Aguarda sinal de ligacao
        if (!ligado) {
            CancelWaitableTimer(hTimer);
            WaitForSingleObject(hEvtLigado, 100);
            continue;
        }

        HANDLE hP = hPipe;
        if (hP == INVALID_HANDLE_VALUE) { Sleep(50); continue; }

        // Lanca leitura assincrona e aguarda: mensagem OU timer
        // (WaitForMultipleObjects e feito dentro de ReadOverlapped)
        MSG_BUF buf;
        ZeroMemory(&buf, sizeof(buf));
        DWORD signal = 0;

        DWORD lidos = ReadOverlapped(hP, &buf, sizeof(buf), hTimer, NULL, &signal);

        if (signal == 1) {
            // --- Timer expirou: alerta terminou ---
            PrintMsg(_T("'---'"));

            // Notifica o central (MSG_CMD tipo 3) - sem resposta necessaria
            if (ligado && hPipe != INVALID_HANDLE_VALUE) {
                MSG_CMD mc = { 3 };
                WriteOverlapped(hP, &mc, sizeof(mc));
            }
            // Timer auto-reset: ja voltou ao estado nao-sinalizado automaticamente
            continue;
        }

        // signal == 0: leitura completou (ou falhou)
        if (lidos == 0) {
            // Pipe partido
            ligado = FALSE;
            hPipe = INVALID_HANDLE_VALUE;
            CloseHandle(hP);
            ResetEvent(hEvtLigado);
            CancelWaitableTimer(hTimer);
            PrintMsg(_T("[AVISO] Ligacao perdida com a Central."));
            continue;
        }

        BYTE tipo = buf.raw[0];

        // --- Tipo 4: NOVO ALERTA ---
        if (tipo == 4) {
            // Cancela timer anterior (se houver alerta ativo)
            CancelWaitableTimer(hTimer);

            // Confirma recepcao ao central (envia MSG_ALERTA de volta)
            WriteOverlapped(hP, &buf.alerta, sizeof(MSG_ALERTA));

            // Mostra alerta na consola
            TCHAR txt[200];
            _stprintf(txt, _T("'%s' (%u seg)"), buf.alerta.msg, buf.alerta.duracao);
            PrintMsg(txt);

            // (Re)inicia waitable timer para a duracao do alerta
            // Valor negativo = relativo (em unidades de 100ns)
            LARGE_INTEGER li;
            li.QuadPart = -(LONGLONG)buf.alerta.duracao * 10000000LL;
            SetWaitableTimer(hTimer, &li, 0, NULL, NULL, 0);
        }
        // --- Tipo 5: CANCELAR ALERTA ---
        else if (tipo == 5) {
            CancelWaitableTimer(hTimer);

            // Confirma ao central (envia MSG_CMD tipo 5 de volta)
            MSG_CMD mc = { 5 };
            WriteOverlapped(hP, &mc, sizeof(mc));

            PrintMsg(_T("'---' (alerta cancelado)"));
        }
        // --- Tipo 6: ENCERRAR PLATAFORMA ---
        else if (tipo == 6) {
            CancelWaitableTimer(hTimer);
            ligado = FALSE;
            hPipe = INVALID_HANDLE_VALUE;
            CloseHandle(hP);
            ResetEvent(hEvtLigado);
            bSair = TRUE;
            PrintMsg(_T("[INFO] Central encerrou. A terminar..."));
            break;
        }
    }

    CloseHandle(hTimer);
    return 0;
}

// =============================================================================
//  MAIN
// =============================================================================
int _tmain(int argc, TCHAR* argv[]) {
    TCHAR cmd[50];
    HKEY  hKey;
    DWORD dw, sz;

#ifdef UNICODE
    (void)_setmode(_fileno(stdin), _O_WTEXT);
    (void)_setmode(_fileno(stdout), _O_WTEXT);
#endif

    InitializeCriticalSection(&csCons);
    hEvtLigado = CreateEvent(NULL, TRUE, FALSE, NULL);

    // --- Leitura/escrita do Registry ---
    if (RegCreateKeyEx(HKEY_CURRENT_USER, _T("Software\\TrabSO2"), 0, NULL,
        REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, NULL, &hKey, &dw) == ERROR_SUCCESS) {

        if (argc > 1) {
            _tcscpy(g_nomePipe, argv[1]);
        }
        else {
            sz = sizeof(g_nomePipe);
            if (RegQueryValueEx(hKey, _T("NPIPE"), NULL, NULL,
                (LPBYTE)g_nomePipe, &sz) != ERROR_SUCCESS) {
                _tprintf(_T("[ERRO] Nome do pipe nao especificado (argv ou registry NPIPE).\n"));
                RegCloseKey(hKey);
                return 1;
            }
        }
        RegSetValueEx(hKey, _T("NPIPE"), 0, REG_SZ,
            (LPBYTE)g_nomePipe, (DWORD)(_tcslen(g_nomePipe) + 1) * sizeof(TCHAR));
        RegCloseKey(hKey);
    }
    else {
        _tprintf(_T("[ERRO] Nao foi possivel aceder ao Registry.\n"));
        return 1;
    }

    _tprintf(_T("=====================================\n"));
    _tprintf(_T(" PLACAR PRONTO. PIPE: %s\n"), g_nomePipe);
    _tprintf(_T(" Comandos: ligar  desligar\n"));
    _tprintf(_T("=====================================\n"));

    // Lanca thread de alertas (usa Overlapped I/O)
    HANDLE hThread = CreateThread(NULL, 0, ThreadAlertas, NULL, 0, NULL);

    // --- Loop do utilizador ---
    while (!bSair) {
        EnterCriticalSection(&csCons);
        _tprintf(_T("CMD> "));
        LeaveCriticalSection(&csCons);

        if (_tscanf_s(_T("%49s"), cmd, (unsigned)_countof(cmd)) == EOF) break;
        if (bSair) break;

        // ===== LIGAR =====
        if (_tcscmp(cmd, _T("ligar")) == 0) {
            if (ligado) {
                _tprintf(_T("[INFO] Ja esta ligado! (ID = %u)\n"), idProprio);
                continue;
            }

            TCHAR fullP[256];
            _stprintf(fullP, _T("\\\\.\\pipe\\%s"), g_nomePipe);

            // Abre o pipe com FILE_FLAG_OVERLAPPED (necessario para Overlapped I/O
            // na ThreadAlertas - slides Named Pipes pag 39)
            HANDLE hP = CreateFile(fullP,
                GENERIC_READ | GENERIC_WRITE,
                0, NULL, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                NULL);

            if (hP == INVALID_HANDLE_VALUE) {
                DWORD err = GetLastError();
                if (err == ERROR_PIPE_BUSY) {
                    if (!WaitNamedPipe(fullP, 5000)) {
                        _tprintf(_T("[ERRO] Central ocupada, tente mais tarde.\n"));
                        continue;
                    }
                    hP = CreateFile(fullP, GENERIC_READ | GENERIC_WRITE,
                        0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
                }
                if (hP == INVALID_HANDLE_VALUE) {
                    _tprintf(_T("[ERRO] Central nao encontrada! (erro %lu)\n"), GetLastError());
                    continue;
                }
            }

            // Garante modo MESSAGE no lado do cliente
            // (CreateFile abre por omissao em modo BYTE - slides NP pag 13)
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(hP, &mode, NULL, NULL);

            // Envia MSG_CMD tipo 1 (ligar) - alternancia simples pedido-resposta
            MSG_CMD mc = { 1 };
            WriteOverlapped(hP, &mc, sizeof(mc));

            // Recebe MSG_ID tipo 7 (identificador atribuido)
            MSG_BUF buf;
            ZeroMemory(&buf, sizeof(buf));
            DWORD signal = 0;
            DWORD lidos = ReadOverlapped(hP, &buf, sizeof(buf), NULL, NULL, &signal);

            if (lidos > 0 && buf.id.tipo == 7) {
                hPipe = hP;
                idProprio = buf.id.identificador;
                ligado = TRUE;
                SetEvent(hEvtLigado);
                _tprintf(_T("[SUCESSO] Identificador = %u\n"), idProprio);
            }
            else {
                CloseHandle(hP);
                _tprintf(_T("[ERRO] Resposta invalida da Central.\n"));
            }
            continue;
        }

        // ===== DESLIGAR =====
        if (_tcscmp(cmd, _T("desligar")) == 0) {
            if (ligado && hPipe != INVALID_HANDLE_VALUE) {
                ligado = FALSE;
                ResetEvent(hEvtLigado);

                HANDLE hP = hPipe;
                hPipe = INVALID_HANDLE_VALUE;

                // Pequena pausa para ThreadAlertas sair do WaitForMultipleObjects
                Sleep(200);

                // Envia MSG_CMD tipo 2 (desligar)
                MSG_CMD mc = { 2 };
                WriteOverlapped(hP, &mc, sizeof(mc));

                // Aguarda confirmacao do central (MSG_CMD tipo 2 de volta)
                MSG_BUF resp;
                DWORD signal = 0;
                ReadOverlapped(hP, &resp, sizeof(resp), NULL, NULL, &signal);

                CloseHandle(hP);
                _tprintf(_T("[INFO] Desligado da Central.\n"));
            }
            bSair = TRUE;
            _tprintf(_T("[INFO] A terminar...\n"));
            break;
        }

        _tprintf(_T("[ERRO] Comando desconhecido: '%s'\n"), cmd);
    }

    if (hThread) {
        WaitForSingleObject(hThread, 3000);
        CloseHandle(hThread);
    }
    CloseHandle(hEvtLigado);
    DeleteCriticalSection(&csCons);
    return 0;
}
