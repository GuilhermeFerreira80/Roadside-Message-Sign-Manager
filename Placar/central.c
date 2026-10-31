// =============================================================================
//  central.c  –  Meta 2 + Meta 3 (SHM para monitor)
//
//  ALTERAÇÕES M3 marcadas com:  // <<< M3
// =============================================================================
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <io.h>
#include <fcntl.h>
#include <stdlib.h>

// =============================================================================
//  MEMÓRIA PARTILHADA (Monitor <-> Central)           // <<< M3
// =============================================================================
#define MSG_LEN      140
#define MAX_PLACARES 20

typedef struct {
    struct {
        DWORD identificador;
        DWORD duracao;
        TCHAR msg[MSG_LEN];
    } placar[MAX_PLACARES];
    BOOL desligar;
} SHM_ALERTA;

// Nomes dos recursos (devem coincidir com os do monitor)
#define SHM_NAME    _T("Local\\SHM_PLACAR")
#define EVT_NAME    _T("Local\\EVT_MONITOR_UPDATE")
#define MTX_NAME    _T("Local\\MTX_PLACAR")

HANDLE      hShmFile = NULL;
SHM_ALERTA* pShm = NULL;
HANDLE      hEvtMon = NULL;   // sinaliza monitores quando algo muda
HANDLE      hMtxShm = NULL;   // protege escrita/leitura da SHM

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
//  ESTRUTURA DE CADA PLACAR
// =============================================================================
typedef struct {
    DWORD  id;
    HANDLE hPipe;
    BOOL   ativo;
    TCHAR  alertaAtual[140];
    DWORD  duracaoAtual;       // <<< M3: guardamos duração para a SHM
} PLACAR_INFO;

// =============================================================================
//  GLOBAIS
// =============================================================================
PLACAR_INFO      placares[MAX_PLACARES];
DWORD            numPlacares = 0;
DWORD            proximoID = 1;
CRITICAL_SECTION csList;
CRITICAL_SECTION csCons;
volatile BOOL    bSair = FALSE;
TCHAR            g_fullPipe[256];

HANDLE hEvtSair = NULL;

// =============================================================================
//  Atualiza a SHM e notifica monitores         // <<< M3
//  DEVE ser chamada FORA de csList
// =============================================================================
static void AtualizaSHMeNotifica(void)
{
    if (!pShm) return;

    WaitForSingleObject(hMtxShm, INFINITE);

    ZeroMemory(pShm, sizeof(SHM_ALERTA));

    EnterCriticalSection(&csList);
    int slot = 0;
    for (DWORD i = 0; i < numPlacares && slot < MAX_PLACARES; i++) {
        if (placares[i].ativo) {
            pShm->placar[slot].identificador = placares[i].id;
            pShm->placar[slot].duracao = placares[i].duracaoAtual;
            _tcsncpy_s(pShm->placar[slot].msg, MSG_LEN,
                placares[i].alertaAtual, _TRUNCATE);
            slot++;
        }
    }
    LeaveCriticalSection(&csList);

    pShm->desligar = bSair ? TRUE : FALSE;

    ReleaseMutex(hMtxShm);

    // Pulsa o evento (manual-reset: set + reset)
    if (hEvtMon) {
        SetEvent(hEvtMon);
        ResetEvent(hEvtMon);
    }
}

// =============================================================================
//  Printf thread-safe
// =============================================================================
void PrintCentral(const TCHAR* fmt, ...) {
    EnterCriticalSection(&csCons);
    va_list args;
    va_start(args, fmt);
    _vtprintf(fmt, args);
    va_end(args);
    _tprintf(_T("\nCentral> "));
    LeaveCriticalSection(&csCons);
}

// =============================================================================
//  Helpers Overlapped: WriteFile e ReadFile assincronos
// =============================================================================
static BOOL PipeWrite(HANDLE hP, const void* buf, DWORD sz) {
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
    return ok;
}

static DWORD PipeRead(HANDLE hP, void* buf, DWORD sz) {
    HANDLE hEvt = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!hEvt) return 0;
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = hEvt;
    DWORD lidos = 0;
    BOOL ok = ReadFile(hP, buf, sz, &lidos, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(hEvt, INFINITE);
        ok = GetOverlappedResult(hP, &ov, &lidos, FALSE);
    }
    CloseHandle(hEvt);
    return ok ? lidos : 0;
}

// =============================================================================
//  Thread por placar
// =============================================================================
DWORD WINAPI ThreadCliente(LPVOID lpParam) {
    DWORD index = *(DWORD*)lpParam;
    free(lpParam);

    HANDLE hP = placares[index].hPipe;

    while (!bSair) {
        MSG_BUF buf;
        ZeroMemory(&buf, sizeof(buf));
        DWORD lidos = PipeRead(hP, &buf, sizeof(buf));

        if (lidos == 0) {
            EnterCriticalSection(&csList);
            if (placares[index].ativo) {
                placares[index].ativo = FALSE;
                CloseHandle(placares[index].hPipe);
                placares[index].hPipe = INVALID_HANDLE_VALUE;
                DWORD id = placares[index].id;
                LeaveCriticalSection(&csList);
                PrintCentral(_T("[INFO] Placar ID %u perdeu ligacao."), id);
                AtualizaSHMeNotifica(); // <<< M3
            }
            else {
                LeaveCriticalSection(&csList);
            }
            break;
        }

        BYTE tipo = buf.raw[0];

        if (tipo == 2) {
            MSG_CMD mc = { 2 };
            PipeWrite(hP, &mc, sizeof(mc));
            EnterCriticalSection(&csList);
            DWORD id = placares[index].id;
            placares[index].ativo = FALSE;
            CloseHandle(hP);
            placares[index].hPipe = INVALID_HANDLE_VALUE;
            LeaveCriticalSection(&csList);
            PrintCentral(_T("[INFO] Placar ID %u desligou-se."), id);
            AtualizaSHMeNotifica(); // <<< M3
            break;
        }
        else if (tipo == 3) {
            // Timer do placar expirou – limpa alerta na lista
            EnterCriticalSection(&csList);
            _tcscpy(placares[index].alertaAtual, _T("---"));
            placares[index].duracaoAtual = 0; // <<< M3
            LeaveCriticalSection(&csList);
            AtualizaSHMeNotifica(); // <<< M3
        }
        else if (tipo == 4) {
            // Eco de confirmação de alerta – sem ação necessária
        }
        else if (tipo == 5) {
            // Eco de confirmação de cancelar – sem ação necessária
        }
    }
    return 0;
}

// =============================================================================
//  Thread que aguarda novas ligações de placares
// =============================================================================
DWORD WINAPI ThreadPipes(LPVOID lpParam) {
    (void)lpParam;

    while (!bSair) {
        EnterCriticalSection(&csList);
        BOOL cheio = (numPlacares >= MAX_PLACARES);
        LeaveCriticalSection(&csList);
        if (cheio) { Sleep(500); continue; }

        HANDLE hP = CreateNamedPipe(
            g_fullPipe,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096, 4096, 0, NULL);

        if (hP == INVALID_HANDLE_VALUE) { Sleep(100); continue; }

        HANDLE hEvtConn = CreateEvent(NULL, TRUE, FALSE, NULL);
        OVERLAPPED ovConn;
        ZeroMemory(&ovConn, sizeof(ovConn));
        ovConn.hEvent = hEvtConn;

        BOOL conn = ConnectNamedPipe(hP, &ovConn);
        DWORD err = GetLastError();

        if (!conn) {
            if (err == ERROR_IO_PENDING) {
                HANDLE wh[2] = { hEvtConn, hEvtSair };
                DWORD wr = WaitForMultipleObjects(2, wh, FALSE, INFINITE);
                if (wr == WAIT_OBJECT_0) {
                    DWORD dummy;
                    GetOverlappedResult(hP, &ovConn, &dummy, FALSE);
                    conn = TRUE;
                }
                else {
                    CancelIo(hP);
                    CloseHandle(hEvtConn);
                    CloseHandle(hP);
                    break;
                }
            }
            else if (err == ERROR_PIPE_CONNECTED) {
                conn = TRUE;
            }
            else {
                CloseHandle(hEvtConn);
                CloseHandle(hP);
                continue;
            }
        }
        CloseHandle(hEvtConn);
        if (!conn) { CloseHandle(hP); continue; }
        if (bSair) { DisconnectNamedPipe(hP); CloseHandle(hP); break; }

        MSG_BUF buf;
        ZeroMemory(&buf, sizeof(buf));
        DWORD lidos = PipeRead(hP, &buf, sizeof(buf));
        if (lidos == 0 || buf.cmd.tipo != 1) {
            DisconnectNamedPipe(hP);
            CloseHandle(hP);
            continue;
        }

        EnterCriticalSection(&csList);
        DWORD idx = numPlacares;
        placares[idx].hPipe = hP;
        placares[idx].id = proximoID++;
        placares[idx].ativo = TRUE;
        placares[idx].duracaoAtual = 0; // <<< M3
        _tcscpy(placares[idx].alertaAtual, _T("---"));
        numPlacares++;

        MSG_ID mi = { 7, placares[idx].id };
        PipeWrite(hP, &mi, sizeof(mi));

        DWORD* pIdx = (DWORD*)malloc(sizeof(DWORD));
        if (pIdx) {
            *pIdx = idx;
            HANDLE hT = CreateThread(NULL, 0, ThreadCliente, pIdx, 0, NULL);
            if (hT) CloseHandle(hT);
        }
        DWORD novoId = placares[idx].id;
        LeaveCriticalSection(&csList);

        PrintCentral(_T("[INFO] Novo Placar ligado! ID atribuido: %u"), novoId);
        AtualizaSHMeNotifica(); // <<< M3
    }
    return 0;
}

// =============================================================================
//  MAIN
// =============================================================================
int _tmain(int argc, TCHAR* argv[]) {
    TCHAR comando[50], msgText[140];
    DWORD dur, idAlvo;

#ifdef UNICODE
    (void)_setmode(_fileno(stdin), _O_WTEXT);
    (void)_setmode(_fileno(stdout), _O_WTEXT);
#endif

    // Garante apenas uma instância
    HANDLE hMutex = CreateMutex(NULL, TRUE, _T("Global\\MutExMinhaCentralUnica"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        _tprintf(_T("[ERRO] A Central ja esta em execucao!\n"));
        CloseHandle(hMutex);
        return 1;
    }

    if (argc < 2) {
        _tprintf(_T("Uso: central.exe <nome_do_pipe>\n"));
        return 1;
    }
    _stprintf(g_fullPipe, _T("\\\\.\\pipe\\%s"), argv[1]);

    InitializeCriticalSection(&csList);
    InitializeCriticalSection(&csCons);
    hEvtSair = CreateEvent(NULL, TRUE, FALSE, NULL);

    // =========================================================================
    //  <<< M3: Criar SHM, evento e mutex para os monitores
    // =========================================================================
    hShmFile = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0, sizeof(SHM_ALERTA),
        SHM_NAME);

    if (!hShmFile) {
        _tprintf(_T("[ERRO] Nao foi possivel criar a SHM.\n"));
        return 1;
    }

    pShm = (SHM_ALERTA*)MapViewOfFile(
        hShmFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);

    if (!pShm) {
        _tprintf(_T("[ERRO] Nao foi possivel mapear a SHM.\n"));
        CloseHandle(hShmFile);
        return 1;
    }

    ZeroMemory(pShm, sizeof(SHM_ALERTA));  // inicializa a zeros

    // Evento manual-reset para notificação dos monitores
    hEvtMon = CreateEvent(NULL, TRUE, FALSE, EVT_NAME);
    if (!hEvtMon) {
        _tprintf(_T("[ERRO] Nao foi possivel criar o evento do monitor.\n"));
        return 1;
    }

    // Mutex para acesso exclusivo à SHM
    hMtxShm = CreateMutex(NULL, FALSE, MTX_NAME);
    if (!hMtxShm) {
        _tprintf(_T("[ERRO] Nao foi possivel criar o mutex da SHM.\n"));
        return 1;
    }
    // =========================================================================

    _tprintf(_T("=====================================\n"));
    _tprintf(_T(" CENTRAL INICIADA. PIPE: %s\n"), argv[1]);
    _tprintf(_T(" Comandos: alerta <msg> <seg> <id|0>  cancelar <id>  listar  encerrar\n"));
    _tprintf(_T("=====================================\n"));

    HANDLE hTPipes = CreateThread(NULL, 0, ThreadPipes, NULL, 0, NULL);

    while (!bSair) {
        EnterCriticalSection(&csCons);
        _tprintf(_T("Central> "));
        LeaveCriticalSection(&csCons);

        if (_tscanf_s(_T("%49s"), comando, (unsigned)_countof(comando)) == EOF) break;
        if (bSair) break;

        // ===== ALERTA =====
        if (_tcscmp(comando, _T("alerta")) == 0) {
            if (_tscanf_s(_T("%139s %u %u"), msgText, (unsigned)_countof(msgText),
                &dur, &idAlvo) != 3) {
                _tprintf(_T("[ERRO] Uso: alerta <msg> <segundos> <id|0=todos>\n"));
                int c; while ((c = _gettchar()) != _T('\n') && c != EOF);
                continue;
            }
            MSG_ALERTA ma;
            ZeroMemory(&ma, sizeof(ma));
            ma.tipo = 4; ma.duracao = dur;
            _tcsncpy(ma.msg, msgText, 139);

            HANDLE pipes[MAX_PLACARES]; DWORD idxArr[MAX_PLACARES]; int n = 0;
            EnterCriticalSection(&csList);
            for (DWORD i = 0; i < numPlacares; i++)
                if (placares[i].ativo && (idAlvo == 0 || placares[i].id == idAlvo))
                {
                    pipes[n] = placares[i].hPipe; idxArr[n++] = i;
                }
            LeaveCriticalSection(&csList);

            int enviados = 0;
            for (int i = 0; i < n; i++) {
                if (PipeWrite(pipes[i], &ma, sizeof(ma))) {
                    EnterCriticalSection(&csList);
                    _tcsncpy(placares[idxArr[i]].alertaAtual, msgText, 139);
                    placares[idxArr[i]].duracaoAtual = dur; // <<< M3
                    LeaveCriticalSection(&csList);
                    enviados++;
                }
            }
            _tprintf(_T("[INFO] Alerta enviado a %d placar(es).\n"), enviados);
            AtualizaSHMeNotifica(); // <<< M3
            continue;
        }

        // ===== CANCELAR =====
        if (_tcscmp(comando, _T("cancelar")) == 0) {
            if (_tscanf_s(_T("%u"), &idAlvo) != 1) {
                _tprintf(_T("[ERRO] Uso: cancelar <id_placar>\n"));
                int c; while ((c = _gettchar()) != _T('\n') && c != EOF);
                continue;
            }
            HANDLE hAlvo = INVALID_HANDLE_VALUE; DWORD idxAlvo = 0;
            EnterCriticalSection(&csList);
            for (DWORD i = 0; i < numPlacares; i++)
                if (placares[i].ativo && placares[i].id == idAlvo)
                {
                    hAlvo = placares[i].hPipe; idxAlvo = i; break;
                }
            LeaveCriticalSection(&csList);

            if (hAlvo != INVALID_HANDLE_VALUE) {
                MSG_CMD mc = { 5 };
                PipeWrite(hAlvo, &mc, sizeof(mc));
                EnterCriticalSection(&csList);
                _tcscpy(placares[idxAlvo].alertaAtual, _T("---"));
                placares[idxAlvo].duracaoAtual = 0; // <<< M3
                LeaveCriticalSection(&csList);
                _tprintf(_T("[INFO] Cancelar enviado ao Placar ID %u.\n"), idAlvo);
                AtualizaSHMeNotifica(); // <<< M3
            }
            else {
                _tprintf(_T("[INFO] Placar ID %u nao encontrado.\n"), idAlvo);
            }
            continue;
        }

        // ===== LISTAR =====
        if (_tcscmp(comando, _T("listar")) == 0) {
            _tprintf(_T("--- PLACARES REGISTADOS ---\n"));
            EnterCriticalSection(&csList);
            int count = 0;
            for (DWORD i = 0; i < numPlacares; i++)
                if (placares[i].ativo) {
                    _tprintf(_T(" -> ID: %u | Alerta: %s\n"),
                        placares[i].id, placares[i].alertaAtual);
                    count++;
                }
            LeaveCriticalSection(&csList);
            if (count == 0) _tprintf(_T(" (nenhum placar ativo)\n"));
            _tprintf(_T("---------------------------\n"));
            continue;
        }

        // ===== ENCERRAR =====
        if (_tcscmp(comando, _T("encerrar")) == 0) {
            _tprintf(_T("[INFO] A encerrar plataforma...\n"));

            MSG_CMD mc = { 6 };
            HANDLE pipes[MAX_PLACARES]; int n = 0;
            EnterCriticalSection(&csList);
            for (DWORD i = 0; i < numPlacares; i++)
                if (placares[i].ativo)
                {
                    pipes[n++] = placares[i].hPipe; placares[i].ativo = FALSE;
                }
            LeaveCriticalSection(&csList);

            for (int i = 0; i < n; i++) {
                PipeWrite(pipes[i], &mc, sizeof(mc));
                FlushFileBuffers(pipes[i]);
                CloseHandle(pipes[i]);
            }
            bSair = TRUE;

            // <<< M3: notifica monitores que a central vai encerrar
            AtualizaSHMeNotifica();

            SetEvent(hEvtSair);
            break;
        }

        _tprintf(_T("[ERRO] Comando desconhecido: '%s'\n"), comando);
    }

    if (hTPipes) { WaitForSingleObject(hTPipes, 3000); CloseHandle(hTPipes); }

    // =========================================================================
    //  <<< M3: Libertação dos recursos SHM / evento / mutex
    // =========================================================================
    if (hMtxShm) { CloseHandle(hMtxShm);   hMtxShm = NULL; }
    if (hEvtMon) { CloseHandle(hEvtMon);   hEvtMon = NULL; }
    if (pShm) { UnmapViewOfFile(pShm);  pShm = NULL; }
    if (hShmFile) { CloseHandle(hShmFile);  hShmFile = NULL; }
    // =========================================================================

    CloseHandle(hEvtSair);
    DeleteCriticalSection(&csList);
    DeleteCriticalSection(&csCons);
    _tprintf(_T("[INFO] Central encerrada.\n"));
    return 0;
}