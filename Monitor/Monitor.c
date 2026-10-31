#define _CRT_SECURE_NO_WARNINGS
#include "framework.h"
#include "Resource.h"
#include <tchar.h>
#include <stdio.h>

#define MAX_PLACARES 20
#define MSG_LEN      140

static TCHAR g_shmName[128] = _T("Local\\SHM_PLACAR");
static TCHAR g_evtName[128] = _T("Local\\EVT_MONITOR_UPDATE");
static TCHAR g_mtxName[128] = _T("Local\\MTX_PLACAR");

typedef struct {
    struct {
        DWORD identificador;
        DWORD duracao;
        TCHAR msg[MSG_LEN];
    } placar[MAX_PLACARES];
    BOOL desligar;
} SHM_ALERTA;

typedef struct {
    DWORD id;
    DWORD duracao;
    TCHAR msg[MSG_LEN];
} ALERTA_LOCAL;

static ALERTA_LOCAL g_alertas[MAX_PLACARES];
static int          g_numAlertas = 0;
static int          g_maxAlertas = 5;
static int          g_pagina = 0;

static HINSTANCE   g_hInst = NULL;
static HWND        g_hWnd = NULL;
static HANDLE      g_hShmFile = NULL;
static SHM_ALERTA* g_pShm = NULL;
static HANDLE      g_hEvt = NULL;
static HANDLE      g_hMtx = NULL;
static HANDLE      g_hEvtSair = NULL;
static HANDLE      g_hThread = NULL;
static CRITICAL_SECTION g_cs;

#define COR_FUNDO        RGB(240,242,245)
#define COR_CARD         RGB(255,255,255)
#define COR_CARD_SOMBRA  RGB(210,215,220)
#define COR_BARRA        RGB(30,120,255)
#define COR_BARRA_SEM    RGB(180,180,190)
#define COR_ID_TEXTO     RGB(30,120,255)
#define COR_MSG_TEXTO    RGB(30,30,40)
#define COR_DUR_TEXTO    RGB(100,110,130)
#define COR_HEADER_FUNDO RGB(30,120,255)
#define COR_HEADER_TEXTO RGB(255,255,255)
#define COR_SEM_ALERTAS  RGB(150,155,165)
#define COR_PAGE_TEXTO   RGB(100,110,130)
#define COR_RODAPE       RGB(225,228,232)

#define MARGEM   16
#define CARD_H   72
#define CARD_GAP  8
#define HEADER_H 48
#define RODAPE_H 32
#define BARRA_W   5

#define CLASS_NAME  _T("MonitorSO2Janela")

// IDs do Resource.h gerado pelo VS
#define IDR_MENU_PRINCIPAL       129
#define IDD_CONFIG               103
#define ID_FICHEIRO_CONFIGURACAO 32774
#define ID_FICHEIRO_ACERCA       32772
#define ID_FICHEIRO_SAIR         32773

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK DlgConfigProc(HWND, UINT, WPARAM, LPARAM);
DWORD   WINAPI   ThreadMonitor(LPVOID);
static BOOL  AbrirSHM(void);
static void  FecharSHM(void);
static void  LerSHM(void);
static void  PintarJanela(HDC hdc, RECT* rc);

// =============================================================================
//  WinMain
// =============================================================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
    LPWSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;

    g_hInst = hInstance;
    InitializeCriticalSection(&g_cs);
    g_hEvtSair = CreateEvent(NULL, TRUE, FALSE, NULL);

    WNDCLASSEX wcl;
    ZeroMemory(&wcl, sizeof(wcl));
    wcl.cbSize = sizeof(WNDCLASSEX);
    wcl.style = CS_HREDRAW | CS_VREDRAW;
    wcl.lpfnWndProc = WndProc;
    wcl.hInstance = hInstance;
    wcl.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcl.hIconSm = LoadIcon(NULL, IDI_APPLICATION);
    wcl.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcl.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcl.lpszMenuName = MAKEINTRESOURCE(IDR_MENU_PRINCIPAL);
    wcl.lpszClassName = CLASS_NAME;

    if (!RegisterClassEx(&wcl)) {
        MessageBox(NULL, _T("Erro ao registar classe."), _T("Monitor"), MB_ICONERROR);
        return 1;
    }

    g_hWnd = CreateWindow(CLASS_NAME, _T("Monitor SO2 - Alertas Ativos"),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        680, 520, NULL, NULL, hInstance, NULL);

    if (!g_hWnd) {
        MessageBox(NULL, _T("Erro ao criar janela."), _T("Monitor"), MB_ICONERROR);
        return 1;
    }

    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);
    AbrirSHM();
    g_hThread = CreateThread(NULL, 0, ThreadMonitor, NULL, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    SetEvent(g_hEvtSair);
    if (g_hThread) { WaitForSingleObject(g_hThread, 3000); CloseHandle(g_hThread); }
    FecharSHM();
    CloseHandle(g_hEvtSair);
    DeleteCriticalSection(&g_cs);
    return (int)msg.wParam;
}

// =============================================================================
//  SHM
// =============================================================================
static BOOL AbrirSHM(void)
{
    FecharSHM();
    g_hShmFile = OpenFileMapping(FILE_MAP_READ, FALSE, g_shmName);
    if (!g_hShmFile) return FALSE;
    g_pShm = (SHM_ALERTA*)MapViewOfFile(g_hShmFile, FILE_MAP_READ, 0, 0, 0);
    if (!g_pShm) { CloseHandle(g_hShmFile); g_hShmFile = NULL; return FALSE; }
    g_hEvt = OpenEvent(SYNCHRONIZE, FALSE, g_evtName);
    g_hMtx = OpenMutex(SYNCHRONIZE, FALSE, g_mtxName);
    if (!g_hEvt || !g_hMtx) { FecharSHM(); return FALSE; }
    return TRUE;
}

static void FecharSHM(void)
{
    if (g_hMtx) { CloseHandle(g_hMtx);     g_hMtx = NULL; }
    if (g_hEvt) { CloseHandle(g_hEvt);     g_hEvt = NULL; }
    if (g_pShm) { UnmapViewOfFile(g_pShm); g_pShm = NULL; }
    if (g_hShmFile) { CloseHandle(g_hShmFile); g_hShmFile = NULL; }
}

static void LerSHM(void)
{
    if (!g_pShm || !g_hMtx) return;
    WaitForSingleObject(g_hMtx, INFINITE);
    ALERTA_LOCAL tmp[MAX_PLACARES];
    int n = 0;
    BOOL desligar = g_pShm->desligar;
    for (int i = 0; i < MAX_PLACARES; i++) {
        if (g_pShm->placar[i].identificador != 0) {
            tmp[n].id = g_pShm->placar[i].identificador;
            tmp[n].duracao = g_pShm->placar[i].duracao;
            _tcsncpy_s(tmp[n].msg, MSG_LEN, g_pShm->placar[i].msg, _TRUNCATE);
            n++;
        }
    }
    ReleaseMutex(g_hMtx);
    EnterCriticalSection(&g_cs);
    g_numAlertas = n;
    if (n > 0) memcpy(g_alertas, tmp, n * sizeof(ALERTA_LOCAL));
    int totalPag = (n == 0) ? 1 : ((n + g_maxAlertas - 1) / g_maxAlertas);
    if (g_pagina >= totalPag) g_pagina = totalPag - 1;
    if (g_pagina < 0)         g_pagina = 0;
    LeaveCriticalSection(&g_cs);
    if (desligar) {
        EnterCriticalSection(&g_cs);
        g_numAlertas = 0; g_pagina = 0;
        LeaveCriticalSection(&g_cs);
        FecharSHM();
    }
}

DWORD WINAPI ThreadMonitor(LPVOID lp)
{
    (void)lp;
    while (TRUE) {
        if (!g_pShm) {
            if (AbrirSHM()) { LerSHM(); InvalidateRect(g_hWnd, NULL, FALSE); }
            else { if (WaitForSingleObject(g_hEvtSair, 1000) == WAIT_OBJECT_0) break; continue; }
        }
        HANDLE wh[2] = { g_hEvt, g_hEvtSair };
        DWORD wr = WaitForMultipleObjects(2, wh, FALSE, INFINITE);
        if (wr == WAIT_OBJECT_0) { LerSHM(); InvalidateRect(g_hWnd, NULL, FALSE); }
        else break;
    }
    return 0;
}

// =============================================================================
//  Pintura
// =============================================================================
static void PintarJanela(HDC hdc, RECT* pRc)
{
    int W = pRc->right - pRc->left;
    int H = pRc->bottom - pRc->top;

    HBRUSH hbFundo = CreateSolidBrush(COR_FUNDO);
    FillRect(hdc, pRc, hbFundo); DeleteObject(hbFundo);
    SetBkMode(hdc, TRANSPARENT);

    RECT rcHeader = { 0, 0, W, HEADER_H };
    HBRUSH hbHeader = CreateSolidBrush(COR_HEADER_FUNDO);
    FillRect(hdc, &rcHeader, hbHeader); DeleteObject(hbHeader);

    HFONT hFontHeader = CreateFont(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, _T("Segoe UI"));
    HFONT hFontId = CreateFont(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, _T("Segoe UI"));
    HFONT hFontMsg = CreateFont(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, _T("Segoe UI"));
    HFONT hFontDur = CreateFont(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, _T("Segoe UI"));

    HFONT hOld = (HFONT)SelectObject(hdc, hFontHeader);
    SetTextColor(hdc, COR_HEADER_TEXTO);
    RECT rcTit = { MARGEM, 0, W - MARGEM, HEADER_H };
    DrawText(hdc, _T("  Monitor SO2  -  Alertas Ativos"), -1, &rcTit, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    EnterCriticalSection(&g_cs);
    int total = g_numAlertas, maxPag = g_maxAlertas, pag = g_pagina;
    int inicio = pag * maxPag, fim = inicio + maxPag;
    if (fim > total) fim = total;
    int totalPag = (total == 0) ? 1 : ((total + maxPag - 1) / maxPag);
    ALERTA_LOCAL snap[MAX_PLACARES];
    if (total > 0) memcpy(snap, g_alertas, total * sizeof(ALERTA_LOCAL));
    LeaveCriticalSection(&g_cs);

    int yCorpo = HEADER_H + MARGEM;
    if (total == 0) {
        SelectObject(hdc, hFontMsg);
        SetTextColor(hdc, COR_SEM_ALERTAS);
        RECT rcSem = { MARGEM, yCorpo + 40, W - MARGEM, yCorpo + 100 };
        DrawText(hdc, _T("Nenhum alerta ativo de momento."), -1, &rcSem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    else {
        for (int i = inicio; i < fim; i++) {
            ALERTA_LOCAL* a = &snap[i];
            int yCard = yCorpo + (i - inicio) * (CARD_H + CARD_GAP);

            RECT rcS = { MARGEM + 2,yCard + 2,W - MARGEM + 2,yCard + CARD_H + 2 };
            HBRUSH hbS = CreateSolidBrush(COR_CARD_SOMBRA);
            FillRect(hdc, &rcS, hbS); DeleteObject(hbS);

            RECT rcCard = { MARGEM,yCard,W - MARGEM,yCard + CARD_H };
            HBRUSH hbC = CreateSolidBrush(COR_CARD);
            FillRect(hdc, &rcCard, hbC); DeleteObject(hbC);

            BOOL temAlerta = (_tcscmp(a->msg, _T("---")) != 0 && _tcslen(a->msg) > 0);
            RECT rcB = { MARGEM,yCard,MARGEM + BARRA_W,yCard + CARD_H };
            HBRUSH hbB = CreateSolidBrush(temAlerta ? COR_BARRA : COR_BARRA_SEM);
            FillRect(hdc, &rcB, hbB); DeleteObject(hbB);

            TCHAR bufId[32];
            _stprintf(bufId, _T("Placar  #%u"), a->id);
            SelectObject(hdc, hFontId);
            SetTextColor(hdc, COR_ID_TEXTO);
            RECT rcId2 = { MARGEM + BARRA_W + 10,yCard + 8,W - MARGEM - 110,yCard + 28 };
            DrawText(hdc, bufId, -1, &rcId2, DT_LEFT | DT_SINGLELINE);

            TCHAR bufDur[32];
            if (temAlerta) _stprintf(bufDur, _T("%u seg"), a->duracao);
            else           _tcscpy(bufDur, _T("-"));
            SelectObject(hdc, hFontDur);
            SetTextColor(hdc, COR_DUR_TEXTO);
            RECT rcDur2 = { W - MARGEM - 105,yCard + 8,W - MARGEM - 8,yCard + 28 };
            DrawText(hdc, bufDur, -1, &rcDur2, DT_RIGHT | DT_SINGLELINE);

            HPEN hPenSep = CreatePen(PS_SOLID, 1, RGB(235, 238, 242));
            HPEN hOldPen = (HPEN)SelectObject(hdc, hPenSep);
            MoveToEx(hdc, MARGEM + BARRA_W + 10, yCard + 32, NULL);
            LineTo(hdc, W - MARGEM - 8, yCard + 32);
            SelectObject(hdc, hOldPen); DeleteObject(hPenSep);

            SelectObject(hdc, hFontMsg);
            SetTextColor(hdc, temAlerta ? COR_MSG_TEXTO : COR_SEM_ALERTAS);
            RECT rcMsg2 = { MARGEM + BARRA_W + 10,yCard + 38,W - MARGEM - 8,yCard + CARD_H - 6 };
            DrawText(hdc, (temAlerta ? a->msg : _T("(sem alerta ativo)")), -1, &rcMsg2, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    RECT rcRod = { 0,H - RODAPE_H,W,H };
    HBRUSH hbRod = CreateSolidBrush(COR_RODAPE);
    FillRect(hdc, &rcRod, hbRod); DeleteObject(hbRod);
    HPEN hPenRod = CreatePen(PS_SOLID, 1, COR_CARD_SOMBRA);
    HPEN hOldPen2 = (HPEN)SelectObject(hdc, hPenRod);
    MoveToEx(hdc, 0, H - RODAPE_H, NULL); LineTo(hdc, W, H - RODAPE_H);
    SelectObject(hdc, hOldPen2); DeleteObject(hPenRod);

    SelectObject(hdc, hFontDur);
    SetTextColor(hdc, COR_PAGE_TEXTO);
    TCHAR bufPag[64];
    if (totalPag > 1)
        _stprintf(bufPag, _T("Pagina %d de %d   |   PageUp / PageDown para navegar"), pag + 1, totalPag);
    else
        _stprintf(bufPag, _T("A mostrar %d de %d alertas"), total, total);
    RECT rcPagTxt = { MARGEM,H - RODAPE_H,W - MARGEM,H };
    DrawText(hdc, bufPag, -1, &rcPagTxt, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdc, hOld);
    DeleteObject(hFontHeader); DeleteObject(hFontId);
    DeleteObject(hFontMsg);    DeleteObject(hFontDur);
}

// =============================================================================
//  Dialog Configuração
// =============================================================================
INT_PTR CALLBACK DlgConfigProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    (void)lParam;
    switch (msg) {
    case WM_INITDIALOG: {
        TCHAR buf[16];
        _stprintf(buf, _T("%d"), g_maxAlertas);
        SetDlgItemText(hDlg, IDC_EDIT_MAX_ALERTAS, buf);
        SetDlgItemText(hDlg, IDC_EDIT_SHM_NAME, g_shmName);
        SetDlgItemText(hDlg, IDC_EDIT_EVT_NAME, g_evtName);
        SetDlgItemText(hDlg, IDC_EDIT_MTX_NAME, g_mtxName);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            TCHAR buf[16];
            GetDlgItemText(hDlg, IDC_EDIT_MAX_ALERTAS, buf, 16);
            int novo = _ttoi(buf);
            if (novo >= 1 && novo <= MAX_PLACARES) g_maxAlertas = novo;
            GetDlgItemText(hDlg, IDC_EDIT_SHM_NAME, g_shmName, 128);
            GetDlgItemText(hDlg, IDC_EDIT_EVT_NAME, g_evtName, 128);
            GetDlgItemText(hDlg, IDC_EDIT_MTX_NAME, g_mtxName, 128);
            AbrirSHM();
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_CLOSE:
        EndDialog(hDlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

// =============================================================================
//  WndProc
// =============================================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hBmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP hBmpOld = (HBITMAP)SelectObject(hdcMem, hBmp);
        PintarJanela(hdcMem, &rc);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hBmpOld);
        DeleteObject(hBmp); DeleteDC(hdcMem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYDOWN:
        if (wParam == VK_NEXT) {
            EnterCriticalSection(&g_cs);
            int tp = (g_numAlertas == 0) ? 1 : ((g_numAlertas + g_maxAlertas - 1) / g_maxAlertas);
            if (g_pagina < tp - 1) g_pagina++;
            LeaveCriticalSection(&g_cs);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        else if (wParam == VK_PRIOR) {
            EnterCriticalSection(&g_cs);
            if (g_pagina > 0) g_pagina--;
            LeaveCriticalSection(&g_cs);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_FICHEIRO_CONFIGURACAO:
            DialogBox(g_hInst, MAKEINTRESOURCE(IDD_CONFIG), hwnd, DlgConfigProc);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case ID_FICHEIRO_ACERCA:
            MessageBox(hwnd,
                _T("Monitor SO2\n\n")
                _T("Aluno 1: Guilherme Marques Ferreira  -  n <2024133874>\n")
                _T("Aluno 2: Gonçalo Neves Fresco  -  n <2024139328>\n\n")
                _T("Sistemas Operativos 2 - 2025/26\n")
                _T("ISEC / DEIS"),
                _T("Acerca"), MB_OK | MB_ICONINFORMATION);
            return 0;
        case ID_FICHEIRO_SAIR:
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}