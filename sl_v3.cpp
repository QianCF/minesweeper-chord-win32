#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#ifdef _MSC_VER
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#endif
#include <random>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <vector>

const int MAX_H = 128, MAX_W = 128;
const int HIGHLIGHT_INSET = 2;
const int INVALIDATE_PAD = 3;

#define IDC_EDIT_H 101
#define IDC_EDIT_W 102
#define IDC_EDIT_N 103
#define IDC_BTN_RESTART 104
#define IDC_STATUS 105
#define IDC_CHK_AUTOFLAG 106

std::mt19937 rng(std::random_device{}());
int RAND(int lo, int hi) {
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(rng);
}

int g_rows = 20, g_cols = 20, g_n = 50;
int Map[MAX_H][MAX_W], Dmap[MAX_H][MAX_W];
bool map[MAX_H][MAX_W];

HWND g_hwnd = nullptr;
HWND g_hwndSettings = nullptr;
HWND g_editRows = nullptr, g_editCols = nullptr, g_editN = nullptr;
HWND g_statusLabel = nullptr;
HWND g_chkAutoFlag = nullptr;
HINSTANCE g_hInst = nullptr;

struct CellCoord { int r, c; };
std::vector<CellCoord> g_openedBatch;

HDC g_memDC = nullptr;
HBITMAP g_memBmp = nullptr;
int g_memW = 0, g_memH = 0;
int g_clientW = 0, g_clientH = 0;
int g_colPos[MAX_W + 1], g_rowPos[MAX_H + 1];
int g_minCell = 16;
bool g_allMines = false;
DWORD g_winStyle = 0;

HBRUSH g_brWhite = nullptr, g_brCover = nullptr, g_brFlag = nullptr, g_brMine = nullptr;
HBRUSH g_brWin = nullptr, g_brWrongFlag = nullptr, g_brMineHidden = nullptr;
HFONT g_fontCell = nullptr;

struct GameState {
    bool firstclick = true;
    bool dead = false;
    bool won = false;
    bool showHover = false;
    int mx = 0, my = 0;
    int downX = 0, downY = 0;
    int hx = -1, hy = -1;
};

GameState g;

void adjustWindowHeightToGrid();
void newGame();
void syncSettingsEdits();
void updateStatusLabel();
bool minesPlaced();
bool revealSingle(int row, int col);
void applyAutoFlagsForBatch();
void finishRevealAction();
void performReveal(int row, int col);
void chordClick(int row, int col);
bool autoFlagEnabled();

int cellCount() { return g_rows * g_cols; }
int maxMines() { return cellCount(); }

void clampMines() {
    if (g_n > maxMines()) g_n = maxMines();
    if (g_n < 1) g_n = 1;
}

bool minesPlaced() {
    return g_allMines || !g.firstclick;
}

int countFlags() {
    int n = 0;
    for (int i = 0; i < g_rows; i++)
        for (int j = 0; j < g_cols; j++)
            if (Dmap[i][j] == -1) n++;
    return n;
}

void updateStatusLabel() {
    if (!g_statusLabel) return;
    const wchar_t* text;
    if (g.won)
        text = L"\u8d62\u4e86";
    else if (g.dead)
        text = L"\u8f93\u4e86";
    else
        text = L"\u8fdb\u884c\u4e2d";
    SetWindowTextW(g_statusLabel, text);
}

void updateGridLayout() {
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    g_clientW = std::max(1, (int)cr.right);
    g_clientH = std::max(1, (int)cr.bottom);

    int baseW = g_clientW / g_cols;
    int remW = g_clientW % g_cols;
    g_colPos[0] = 0;
    for (int c = 0; c < g_cols; c++) {
        int w = baseW + (c < remW ? 1 : 0);
        g_colPos[c + 1] = g_colPos[c] + w;
    }

    int baseH = g_clientH / g_rows;
    int remH = g_clientH % g_rows;
    g_rowPos[0] = 0;
    for (int r = 0; r < g_rows; r++) {
        int h = baseH + (r < remH ? 1 : 0);
        g_rowPos[r + 1] = g_rowPos[r] + h;
    }

    g_minCell = g_colPos[1] - g_colPos[0];
    for (int c = 1; c < g_cols; c++)
        g_minCell = std::min(g_minCell, g_colPos[c + 1] - g_colPos[c]);
    for (int r = 0; r < g_rows; r++)
        g_minCell = std::min(g_minCell, g_rowPos[r + 1] - g_rowPos[r]);
    g_minCell = std::max(1, g_minCell);
}

void cellRect(int row, int col, RECT* r) {
    r->left = g_colPos[col];
    r->top = g_rowPos[row];
    r->right = g_colPos[col + 1];
    r->bottom = g_rowPos[row + 1];
}

void boardPixelRect(RECT* r) {
    r->left = 0;
    r->top = 0;
    r->right = g_clientW;
    r->bottom = g_clientH;
}

int posToCol(int x) {
    for (int c = 0; c < g_cols; c++)
        if (x >= g_colPos[c] && x < g_colPos[c + 1]) return c;
    return -1;
}

int posToRow(int y) {
    for (int r = 0; r < g_rows; r++)
        if (y >= g_rowPos[r] && y < g_rowPos[r + 1]) return r;
    return -1;
}

void invalidateCell(int row, int col) {
    if (!g_hwnd) return;
    if (row < 0 || row >= g_rows || col < 0 || col >= g_cols) return;
    RECT r, board;
    cellRect(row, col, &r);
    InflateRect(&r, INVALIDATE_PAD, INVALIDATE_PAD);
    boardPixelRect(&board);
    if (IntersectRect(&r, &r, &board))
        InvalidateRect(g_hwnd, &r, FALSE);
}

void invalidateBoard() {
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

COLORREF numColor(int num) {
    static const COLORREF colors[] = {
        RGB(0, 0, 255), RGB(0, 128, 0), RGB(255, 0, 0), RGB(0, 0, 128),
        RGB(128, 0, 0), RGB(0, 128, 128), RGB(0, 0, 0), RGB(128, 128, 128),
    };
    if (num < 1 || num > 8) return RGB(0, 0, 0);
    return colors[num - 1];
}

void computeMapNumbers() {
    for (int i = 0; i < g_rows; i++)
        for (int j = 0; j < g_cols; j++)
            if (map[i][j])
                Map[i][j] = 9;
            else {
                int cnt = 0;
                for (int k = std::max(i - 1, 0); k <= std::min(i + 1, g_rows - 1); k++)
                    for (int m = std::max(j - 1, 0); m <= std::min(j + 1, g_cols - 1); m++)
                        if (map[k][m]) cnt++;
                Map[i][j] = cnt;
            }
}

void setupAllMinesBoard() {
    memset(map, 0, sizeof(map));
    for (int i = 0; i < g_rows; i++)
        for (int j = 0; j < g_cols; j++)
            map[i][j] = true;
    computeMapNumbers();
}

bool inClickSafe3x3(int col, int row, int clickCol, int clickRow) {
    return abs(col - clickCol) <= 1 && abs(row - clickRow) <= 1;
}

bool firstClickSafe3x3() {
    return cellCount() - g_n > 9;
}

void generateMinesAfterClick(int clickCol, int clickRow) {
    memset(map, 0, sizeof(map));
    const int cells = cellCount();
    const bool safe3x3 = firstClickSafe3x3();

    std::vector<int> slots;
    slots.reserve(cells);
    for (int i = 0; i < cells; i++) {
        const int r = i / g_cols, c = i % g_cols;
        if (safe3x3) {
            if (inClickSafe3x3(c, r, clickCol, clickRow)) continue;
        } else if (r == clickRow && c == clickCol) {
            continue;
        }
        slots.push_back(i);
    }

    std::shuffle(slots.begin(), slots.end(), rng);
    const int mines = std::min(g_n, (int)slots.size());
    for (int k = 0; k < mines; k++) {
        int idx = slots[k];
        map[idx / g_cols][idx % g_cols] = true;
    }
    computeMapNumbers();
}

void drawCellNumber(HDC hdc, const RECT& cell, int num) {
    if (num <= 0 || num >= 9) return;
    HFONT old = (HFONT)SelectObject(hdc, g_fontCell);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, numColor(num));
    wchar_t buf[8];
    swprintf(buf, 8, L"%d", num);
    DrawTextW(hdc, buf, -1, (RECT*)&cell, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, old);
}

void drawFlagMark(HDC hdc, const RECT& cell, HBRUSH br) {
    int pad = std::max(2, g_minCell / 4);
    RECT r = {cell.left + pad, cell.top + pad, cell.right - pad, cell.bottom - pad};
    FillRect(hdc, &r, br);
}

void drawCell(HDC hdc, int row, int col) {
    RECT cell;
    cellRect(row, col, &cell);
    const int dm = Dmap[row][col];
    const bool isMine = minesPlaced() && Map[row][col] == 9;

    if (g.won) {
        if (dm == -1) {
            FillRect(hdc, &cell, isMine ? g_brWin : g_brWrongFlag);
            drawFlagMark(hdc, cell, isMine ? g_brFlag : g_brMine);
        } else if (dm == 1 && !isMine) {
            FillRect(hdc, &cell, g_brWin);
            drawCellNumber(hdc, cell, Map[row][col]);
        } else if (isMine) {
            FillRect(hdc, &cell, g_brMineHidden);
        } else {
            FillRect(hdc, &cell, g_brCover);
        }
        return;
    }

    if (g.dead) {
        if (isMine) {
            FillRect(hdc, &cell, g_brMine);
        } else if (dm == -1) {
            FillRect(hdc, &cell, g_brWrongFlag);
            drawFlagMark(hdc, cell, g_brMine);
        } else if (dm == 1) {
            FillRect(hdc, &cell, g_brWhite);
            drawCellNumber(hdc, cell, Map[row][col]);
        } else {
            FillRect(hdc, &cell, g_brCover);
        }
        return;
    }

    FillRect(hdc, &cell, g_brWhite);
    if (dm == 1 && minesPlaced() && Map[row][col] > 0 && Map[row][col] < 9) {
        drawCellNumber(hdc, cell, Map[row][col]);
    } else if (dm != 1) {
        FillRect(hdc, &cell, g_brCover);
        if (dm == -1)
            drawFlagMark(hdc, cell, g_brFlag);
    }
}

void refreshCell(int row, int col) {
    if (!g_memDC) return;
    drawCell(g_memDC, row, col);
    invalidateCell(row, col);
}

void redrawAllCells() {
    if (!g_memDC) return;
    for (int i = 0; i < g_rows; i++)
        for (int j = 0; j < g_cols; j++)
            drawCell(g_memDC, i, j);
}

void freeBackBuffer() {
    if (g_memDC) DeleteDC(g_memDC);
    if (g_memBmp) DeleteObject(g_memBmp);
    g_memDC = nullptr;
    g_memBmp = nullptr;
    g_memW = g_memH = 0;
}

bool resizeBackBuffer() {
    if (!g_hwnd) return false;
    updateGridLayout();
    int nw = g_clientW;
    int nh = g_clientH;
    if (nw < 1 || nh < 1) return false;

    if (g_memDC && nw == g_memW && nh == g_memH) return true;

    freeBackBuffer();
    HDC hdc = GetDC(g_hwnd);
    g_memDC = CreateCompatibleDC(hdc);
    g_memBmp = CreateCompatibleBitmap(hdc, nw, nh);
    ReleaseDC(g_hwnd, hdc);
    if (!g_memDC || !g_memBmp) return false;

    SelectObject(g_memDC, g_memBmp);
    g_memW = nw;
    g_memH = nh;
    redrawAllCells();
    return true;
}

void updateFonts() {
    if (g_fontCell) DeleteObject(g_fontCell);
    int cellH = std::max(12, g_minCell - 4);
    g_fontCell = CreateFontW(cellH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH, L"Arial");
}

void revealAll() {
    for (int i = 0; i < g_rows; i++)
        for (int j = 0; j < g_cols; j++)
            Dmap[i][j] = 1;
}

void gameDead() {
    g.dead = true;
    revealAll();
    redrawAllCells();
    invalidateBoard();
    updateStatusLabel();
}

void gameWin() {
    if (g.won) return;
    g.won = true;
    for (int i = 0; i < g_rows; i++)
        for (int j = 0; j < g_cols; j++)
            if (!map[i][j] && Dmap[i][j] == 0)
                Dmap[i][j] = 1;
    redrawAllCells();
    invalidateBoard();
    updateStatusLabel();
}

bool checkFlagWin() {
    if (countFlags() != g_n) return false;
    for (int i = 0; i < g_rows; i++)
        for (int j = 0; j < g_cols; j++)
            if (Dmap[i][j] == -1 && !map[i][j])
                return false;
    return true;
}

bool checkRevealWin() {
    for (int i = 0; i < g_rows; i++)
        for (int j = 0; j < g_cols; j++)
            if (!map[i][j] && Dmap[i][j] != 1)
                return false;
    return true;
}

bool checkWin() {
    if (!minesPlaced()) return false;
    return checkFlagWin() || checkRevealWin();
}

bool autoFlagEnabled() {
    if (!g_chkAutoFlag) return true;
    return IsDlgButtonChecked(g_hwndSettings, IDC_CHK_AUTOFLAG) == BST_CHECKED;
}

bool autoFlagAround(int row, int col) {
    if (!autoFlagEnabled() || !minesPlaced()) return false;
    const int num = Map[row][col];
    if (num < 1 || num > 8 || Dmap[row][col] != 1) return false;

    int flags = 0;
    CellCoord toFlag[8];
    int hidden = 0;

    for (int i = -1; i <= 1; i++) {
        for (int j = -1; j <= 1; j++) {
            if (i == 0 && j == 0) continue;
            const int nr = row + i, nc = col + j;
            if (nr < 0 || nr >= g_rows || nc < 0 || nc >= g_cols) continue;
            if (Dmap[nr][nc] == 1) continue;
            if (Dmap[nr][nc] == -1)
                flags++;
            else if (Dmap[nr][nc] == 0)
                toFlag[hidden++] = {nr, nc};
        }
    }

    const int notOpen = flags + hidden;
    if (notOpen != num || hidden == 0 || hidden != num - flags) return false;

    for (int k = 0; k < hidden; k++) {
        Dmap[toFlag[k].r][toFlag[k].c] = -1;
        refreshCell(toFlag[k].r, toFlag[k].c);
    }
    return true;
}

void collect3x3(int row, int col, std::vector<CellCoord>& out,
                std::vector<unsigned char>& mark) {
    for (int i = -1; i <= 1; i++) {
        for (int j = -1; j <= 1; j++) {
            const int nr = row + i, nc = col + j;
            if (nr < 0 || nr >= g_rows || nc < 0 || nc >= g_cols) continue;
            const int idx = nr * g_cols + nc;
            if (mark[idx]) continue;
            mark[idx] = 1;
            out.push_back({nr, nc});
        }
    }
}

void applyAutoFlagsForBatch() {
    if (!autoFlagEnabled()) return;

    std::vector<unsigned char> mark(g_rows * g_cols, 0);
    std::vector<CellCoord> wave;
    wave.reserve(g_openedBatch.size() * 9);

    for (const CellCoord& p : g_openedBatch)
        collect3x3(p.r, p.c, wave, mark);

    while (!wave.empty()) {
        bool anySuccess = false;
        std::vector<unsigned char> nextMark(g_rows * g_cols, 0);
        std::vector<CellCoord> nextWave;
        nextWave.reserve(wave.size() * 9);

        for (const CellCoord& cell : wave) {
            if (!autoFlagAround(cell.r, cell.c)) continue;
            anySuccess = true;
            collect3x3(cell.r, cell.c, nextWave, nextMark);
        }

        if (!anySuccess) break;
        wave.swap(nextWave);
    }
}

bool revealSingle(int row, int col) {
    if (!minesPlaced()) return false;
    if (row < 0 || row >= g_rows || col < 0 || col >= g_cols) return false;
    if (Dmap[row][col] == 1 || Dmap[row][col] == -1) return false;

    if (Map[row][col] == 9) {
        Dmap[row][col] = 1;
        refreshCell(row, col);
        gameDead();
        return true;
    }

    Dmap[row][col] = 1;
    g_openedBatch.push_back({row, col});
    refreshCell(row, col);

    if (Map[row][col] == 0) {
        for (int i = -1; i <= 1; i++)
            for (int j = -1; j <= 1; j++)
                if (revealSingle(row + i, col + j))
                    return true;
    }
    return false;
}

void finishRevealAction() {
    applyAutoFlagsForBatch();
    g_openedBatch.clear();
    if (!g.dead && minesPlaced() && checkWin())
        gameWin();
}

void performReveal(int row, int col) {
    g_openedBatch.clear();
    if (revealSingle(row, col)) {
        g_openedBatch.clear();
        return;
    }
    finishRevealAction();
}

void chordClick(int row, int col) {
    if (!minesPlaced() || g.dead || g.won) return;
    const int num = Map[row][col];
    if (num < 1 || num > 8 || Dmap[row][col] != 1) return;

    int flags = 0;
    CellCoord toOpen[8];
    int openCnt = 0;

    for (int i = -1; i <= 1; i++) {
        for (int j = -1; j <= 1; j++) {
            if (i == 0 && j == 0) continue;
            const int nr = row + i, nc = col + j;
            if (nr < 0 || nr >= g_rows || nc < 0 || nc >= g_cols) continue;
            if (Dmap[nr][nc] == -1)
                flags++;
            else if (Dmap[nr][nc] == 0)
                toOpen[openCnt++] = {nr, nc};
        }
    }
    if (flags < num) return;

    g_openedBatch.clear();
    for (int k = 0; k < openCnt; k++) {
        if (revealSingle(toOpen[k].r, toOpen[k].c)) {
            g_openedBatch.clear();
            return;
        }
    }
    finishRevealAction();
}

void newGame() {
    g.firstclick = true;
    g.dead = false;
    g.won = false;
    g.showHover = false;
    g.hx = g.hy = -1;
    clampMines();
    g_allMines = (g_n >= cellCount());
    g_openedBatch.clear();
    memset(Dmap, 0, sizeof(Dmap));
    memset(Map, 0, sizeof(Map));
    if (g_allMines)
        setupAllMinesBoard();
    else
        memset(map, 0, sizeof(map));
    resizeBackBuffer();
    redrawAllCells();
    invalidateBoard();
    updateStatusLabel();
}

void syncSettingsEdits() {
    if (!g_editRows) return;
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", g_rows);
    SetWindowTextW(g_editRows, buf);
    swprintf(buf, 16, L"%d", g_cols);
    SetWindowTextW(g_editCols, buf);
    swprintf(buf, 16, L"%d", g_n);
    SetWindowTextW(g_editN, buf);
}

bool readEditInt(HWND edit, int* out) {
    wchar_t buf[32] = {};
    GetWindowTextW(edit, buf, 32);
    if (buf[0] == L'\0') return false;
    int v = _wtoi(buf);
    if (v < 1) return false;
    *out = v;
    return true;
}

bool applyConfigFromSettings() {
    int h, w, n;
    if (!readEditInt(g_editRows, &h) || !readEditInt(g_editCols, &w) || !readEditInt(g_editN, &n))
        return false;

    g_rows = std::max(1, std::min(MAX_H, h));
    g_cols = std::max(1, std::min(MAX_W, w));
    g_n = n;
    clampMines();
    syncSettingsEdits();
    adjustWindowHeightToGrid();
    return true;
}

void onRestartClicked() {
    if (!applyConfigFromSettings()) {
        MessageBoxW(g_hwndSettings,
            L"\u8bf7\u8f93\u5165\u6709\u6548\u6570\u5b57\uff08\u5747 >= 1\uff09",
            L"\u626b\u96f7", MB_OK | MB_ICONWARNING);
        syncSettingsEdits();
        return;
    }
    newGame();
}

void drawHighlight(HDC hdc, const RECT& paintArea) {
    if (!g.showHover || g.hx < 0 || g.hy < 0) return;

    int hx = posToCol(g.mx), hy = posToRow(g.my);
    if (hx < 0 || hx >= g_cols || hy < 0 || hy >= g_rows) return;
    if (hx != g.hx || hy != g.hy) return;

    RECT hr;
    cellRect(hy, hx, &hr);
    RECT intersect;
    if (!IntersectRect(&intersect, &paintArea, &hr)) return;

    InflateRect(&hr, -HIGHLIGHT_INSET, -HIGHLIGHT_INSET);
    if (hr.left >= hr.right || hr.top >= hr.bottom) return;
    FrameRect(hdc, &hr, (HBRUSH)GetStockObject(WHITE_BRUSH));
}

void screenToGrid(int px, int py, int* gx, int* gy) {
    *gx = posToCol(px);
    *gy = posToRow(py);
}

void onLeftUp(int gx, int gy, int downGx, int downGy) {
    if (g.dead) {
        newGame();
        return;
    }
    if (g.won) return;
    if (gx != downGx || gy != downGy) return;
    if (gx < 0 || gx >= g_cols || gy < 0 || gy >= g_rows) return;
    if (Dmap[gy][gx] == -1) return;

    if (Dmap[gy][gx] == 1) {
        chordClick(gy, gx);
        return;
    }

    if (!g.firstclick) {
        if (minesPlaced() && Map[gy][gx] == 9) {
            gameDead();
        } else {
            performReveal(gy, gx);
        }
    } else {
        g.firstclick = false;
        if (g_allMines) {
            gameDead();
            return;
        }
        generateMinesAfterClick(gx, gy);
        if (Map[gy][gx] == 9) {
            gameDead();
            return;
        }
        performReveal(gy, gx);
    }
}

void onRightDown(int gx, int gy) {
    if (g.dead) {
        newGame();
        return;
    }
    if (g.won) return;
    if (gx < 0 || gx >= g_cols || gy < 0 || gy >= g_rows) return;
    if (Dmap[gy][gx] == 1) return;

    if (Dmap[gy][gx] == 0)
        Dmap[gy][gx] = -1;
    else if (Dmap[gy][gx] == -1)
        Dmap[gy][gx] = 0;
    refreshCell(gy, gx);
    if (minesPlaced() && checkWin()) gameWin();
}

void updateHover(int mx, int my) {
    g.mx = mx;
    g.my = my;
    g.showHover = true;

    int hx, hy;
    screenToGrid(mx, my, &hx, &hy);
    if (hx < 0 || hx >= g_cols || hy < 0 || hy >= g_rows) {
        if (g.hx >= 0) invalidateCell(g.hy, g.hx);
        g.hx = g.hy = -1;
        return;
    }

    if (hx == g.hx && hy == g.hy) return;
    if (g.hx >= 0) invalidateCell(g.hy, g.hx);
    g.hx = hx;
    g.hy = hy;
    invalidateCell(g.hy, g.hx);
}

void adjustWindowHeightToGrid() {
    if (!g_hwnd) return;

    RECT wr;
    GetWindowRect(g_hwnd, &wr);
    const int keepWinW = wr.right - wr.left;

    RECT templ = {0, 0, 100, 100};
    AdjustWindowRect(&templ, g_winStyle, FALSE);
    const int ncW = (templ.right - templ.left) - 100;
    const int ncH = (templ.bottom - templ.top) - 100;

    int cliW = std::max(g_cols, keepWinW - ncW);
    int cliH = (int)((long long)cliW * g_rows / g_cols + 0.5);
    cliH = std::max(g_rows, cliH);

    SetWindowPos(g_hwnd, nullptr, 0, 0, cliW + ncW, cliH + ncH,
        SWP_NOMOVE | SWP_NOZORDER);
}

void enforceSizingAspect(HWND hwnd, RECT* r, WPARAM edge) {
    RECT wr = {0, 0, 100, 100};
    AdjustWindowRect(&wr, g_winStyle, FALSE);
    int ncW = (wr.right - wr.left) - 100;
    int ncH = (wr.bottom - wr.top) - 100;

    int winW = r->right - r->left;
    int winH = r->bottom - r->top;
    int cliW = std::max(1, winW - ncW);
    int cliH = std::max(1, winH - ncH);

    const double target = (double)g_cols / g_rows;

    bool horiz = (edge == WMSZ_LEFT || edge == WMSZ_RIGHT ||
                  edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT ||
                  edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT);
    bool vert = (edge == WMSZ_TOP || edge == WMSZ_BOTTOM ||
                 edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT ||
                 edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT);

    if (horiz && !vert)
        cliH = (int)(cliW / target + 0.5);
    else
        cliW = (int)(cliH * target + 0.5);
    cliW = std::max(g_cols, cliW);
    cliH = std::max(g_rows, cliH);

    winW = cliW + ncW;
    winH = cliH + ncH;

    int dx = winW - (r->right - r->left);
    int dy = winH - (r->bottom - r->top);

    switch (edge) {
    case WMSZ_LEFT: case WMSZ_TOPLEFT: case WMSZ_BOTTOMLEFT:
        r->left -= dx; break;
    default:
        r->right += dx; break;
    }
    switch (edge) {
    case WMSZ_TOP: case WMSZ_TOPLEFT: case WMSZ_TOPRIGHT:
        r->top -= dy; break;
    default:
        r->bottom += dy; break;
    }
}

void closeApp(HWND closed) {
    if (closed != g_hwnd && g_hwnd && IsWindow(g_hwnd))
        DestroyWindow(g_hwnd);
    if (closed != g_hwndSettings && g_hwndSettings && IsWindow(g_hwndSettings))
        DestroyWindow(g_hwndSettings);
    PostQuitMessage(0);
}

HWND createEdit(HWND parent, int id, int x, int y, int w, int h, int value) {
    wchar_t buf[16];
    swprintf(buf, 16, L"%d", value);
    return CreateWindowW(L"EDIT", buf,
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, g_hInst, nullptr);
}

HWND createLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    return CreateWindowW(L"STATIC", text,
        WS_CHILD | WS_VISIBLE,
        x, y, w, h, parent, nullptr, g_hInst, nullptr);
}

bool createSettingsWindow(int x, int y) {
    g_hwndSettings = CreateWindowW(
        L"MinesweeperSettings", L"\u8bbe\u7f6e",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, 220, 260,
        nullptr, nullptr, g_hInst, nullptr);
    if (!g_hwndSettings) return false;

    const int lx = 12, ex = 88, ew = 100, rowH = 32;
    int y0 = 16;

    createLabel(g_hwndSettings, L"\u884c\u6570:", lx, y0 + 4, 70, 22);
    g_editRows = createEdit(g_hwndSettings, IDC_EDIT_H, ex, y0, ew, 24, g_rows);
    y0 += rowH;

    createLabel(g_hwndSettings, L"\u5217\u6570:", lx, y0 + 4, 70, 22);
    g_editCols = createEdit(g_hwndSettings, IDC_EDIT_W, ex, y0, ew, 24, g_cols);
    y0 += rowH;

    createLabel(g_hwndSettings, L"\u96f7\u6570:", lx, y0 + 4, 70, 22);
    g_editN = createEdit(g_hwndSettings, IDC_EDIT_N, ex, y0, ew, 24, g_n);
    y0 += rowH;

    g_statusLabel = createLabel(g_hwndSettings, L"\u8fdb\u884c\u4e2d", lx, y0 + 4, 176, 22);
    y0 += rowH;

    g_chkAutoFlag = CreateWindowW(L"BUTTON",
        L"\u81ea\u52a8\u6807\u96f7",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        lx, y0, 176, 22,
        g_hwndSettings, (HMENU)(INT_PTR)IDC_CHK_AUTOFLAG, g_hInst, nullptr);
    SendMessageW(g_chkAutoFlag, BM_SETCHECK, BST_CHECKED, 0);
    y0 += rowH;

    CreateWindowW(L"BUTTON", L"\u91cd\u5f00",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        lx, y0, 176, 30,
        g_hwndSettings, (HMENU)(INT_PTR)IDC_BTN_RESTART, g_hInst, nullptr);

    return true;
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_RESTART && HIWORD(wParam) == BN_CLICKED) {
            onRestartClicked();
            return 0;
        }
        break;
    case WM_DESTROY:
        g_hwndSettings = nullptr;
        closeApp(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_brWhite = CreateSolidBrush(RGB(255, 255, 255));
        g_brCover = CreateSolidBrush(RGB(70, 70, 70));
        g_brFlag = CreateSolidBrush(RGB(250, 30, 30));
        g_brMine = CreateSolidBrush(RGB(250, 0, 0));
        g_brWin = CreateSolidBrush(RGB(180, 240, 180));
        g_brWrongFlag = CreateSolidBrush(RGB(255, 200, 60));
        g_brMineHidden = CreateSolidBrush(RGB(120, 120, 120));
        g_winStyle = (DWORD)GetWindowLong(hwnd, GWL_STYLE);
        updateFonts();
        newGame();
        return 0;
    case WM_DESTROY:
        DeleteObject(g_brWhite);
        DeleteObject(g_brCover);
        DeleteObject(g_brFlag);
        DeleteObject(g_brMine);
        DeleteObject(g_brWin);
        DeleteObject(g_brWrongFlag);
        DeleteObject(g_brMineHidden);
        if (g_fontCell) DeleteObject(g_fontCell);
        freeBackBuffer();
        g_hwnd = nullptr;
        closeApp(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZING:
        enforceSizingAspect(hwnd, (RECT*)lParam, wParam);
        return TRUE;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            resizeBackBuffer();
            updateFonts();
            redrawAllCells();
            invalidateBoard();
        }
        return 0;
    case WM_MOUSEMOVE:
        updateHover(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_LBUTTONDOWN:
        g.downX = LOWORD(lParam);
        g.downY = HIWORD(lParam);
        return 0;
    case WM_LBUTTONUP: {
        int gx, gy, dgx, dgy;
        screenToGrid(g.mx, g.my, &gx, &gy);
        screenToGrid(g.downX, g.downY, &dgx, &dgy);
        onLeftUp(gx, gy, dgx, dgy);
        return 0;
    }
    case WM_RBUTTONDOWN: {
        int gx, gy;
        screenToGrid(LOWORD(lParam), HIWORD(lParam), &gx, &gy);
        onRightDown(gx, gy);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_memDC) {
            const RECT& pr = ps.rcPaint;
            BitBlt(hdc, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top,
                   g_memDC, pr.left, pr.top, SRCCOPY);
            drawHighlight(hdc, pr);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInst = hInst;

    WNDCLASSW wcGame = {};
    wcGame.lpfnWndProc = GameWndProc;
    wcGame.hInstance = hInst;
    wcGame.lpszClassName = L"Minesweeper";
    wcGame.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcGame.hbrBackground = nullptr;
    RegisterClassW(&wcGame);

    WNDCLASSW wcSet = {};
    wcSet.lpfnWndProc = SettingsWndProc;
    wcSet.hInstance = hInst;
    wcSet.lpszClassName = L"MinesweeperSettings";
    wcSet.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcSet.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wcSet);

    g_winStyle = WS_OVERLAPPEDWINDOW;
    RECT gr = {0, 0, g_cols * 32, g_rows * 32};
    AdjustWindowRect(&gr, g_winStyle, FALSE);
    int gameW = gr.right - gr.left;
    int gameH = gr.bottom - gr.top;

    g_hwnd = CreateWindowW(
        L"Minesweeper", L"\u626b\u96f7",
        g_winStyle,
        CW_USEDEFAULT, CW_USEDEFAULT,
        gameW, gameH,
        nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) return 1;

    RECT gamePos;
    GetWindowRect(g_hwnd, &gamePos);
    if (!createSettingsWindow(gamePos.right + 8, gamePos.top)) return 1;

    ShowWindow(g_hwnd, nCmdShow);
    ShowWindow(g_hwndSettings, nCmdShow);
    UpdateWindow(g_hwnd);
    UpdateWindow(g_hwndSettings);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
