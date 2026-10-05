/* tui.cpp -- drawing helpers over ncursesw.

   Copyright (C) 2026 mb6ockatf

   This file is part of raspdash.

   raspdash is free software: you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation, either version 3 of the License, or (at your
   option) any later version.

   raspdash is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with raspdash.  If not, see <https://www.gnu.org/licenses/>.  */

#include <config.h>

#include "tui.hpp"

#include <algorithm>
#include <climits>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cwchar>

#include <langinfo.h>

namespace tui
{
namespace
{
attr_t role_attr[ROLE_COUNT];
bool nerd_icons_ = true;

std::wstring
widen (std::string_view s)
{
    std::wstring out;
    std::mbstate_t st{};
    const char *p = s.data ();
    const char *end = p + s.size ();
    while (p < end)
        {
            wchar_t wc;
            std::size_t n = std::mbrtowc (&wc, p, end - p, &st);
            if (n == static_cast<std::size_t> (-1)
                    || n == static_cast<std::size_t> (-2))
                {
                    wc = L'�';
                    n = 1;
                    st = {};
                }
            else if (n == 0)
                n = 1;
            out += wc;
            p += n;
        }
    return out;
}

std::string
narrow (std::wstring_view ws)
{
    std::string out;
    std::mbstate_t st{};
    char buf[MB_LEN_MAX];
    for (wchar_t wc : ws)
        {
            std::size_t n = std::wcrtomb (buf, wc, &st);
            if (n != static_cast<std::size_t> (-1))
                out.append (buf, n);
        }
    return out;
}

int
wc_width (wchar_t wc)
{
    int w = ::wcwidth (wc);
    return w < 0 ? 0 : w;
}

void
define (Role r, short fg, short bg, attr_t extra = 0)
{
    init_pair (r, fg, bg);
    role_attr[r] = COLOR_PAIR (r) | extra;
}
}

void
set_nerd_icons (bool on)
{
    nerd_icons_ = on;
}

bool
nerd_icons ()
{
    return nerd_icons_;
}

const char *
icon (const char *nerd, const char *fallback)
{
    return nerd_icons_ ? nerd : fallback;
}

attr_t
attr (Role role)
{
    return role_attr[role];
}

Role
level (double frac, double warn, double bad)
{
    if (frac >= bad)
        return BAD;
    if (frac >= warn)
        return WARN;
    return GOOD;
}

void
init ()
{
    std::setlocale (LC_ALL, "");

    if (std::string_view (nl_langinfo (CODESET)) != "UTF-8")
        std::setlocale (LC_CTYPE, "C.UTF-8");

    setenv ("ESCDELAY", "25", 0);
    initscr ();
    cbreak ();
    noecho ();
    keypad (stdscr, TRUE);
    curs_set (0);
    if (has_colors ())
        {
            start_color ();
            use_default_colors ();
            bool rich = COLORS >= 256;
            define (NORMAL, -1, -1);
            define (DIM, rich ? 244 : COLOR_WHITE, -1, rich ? 0 : A_DIM);
            define (TITLE, rich ? 75 : COLOR_CYAN, -1, A_BOLD);
            define (ACCENT, rich ? 214 : COLOR_YELLOW, -1, A_BOLD);
            define (GOOD, rich ? 114 : COLOR_GREEN, -1);
            define (WARN, rich ? 221 : COLOR_YELLOW, -1);
            define (BAD, rich ? 203 : COLOR_RED, -1, A_BOLD);
            define (FRAME, rich ? 240 : COLOR_BLUE, -1);
            define (TAB_ON, rich ? 16 : COLOR_BLACK, rich ? 75 : COLOR_CYAN,
                    A_BOLD);
            define (TAB_OFF, rich ? 250 : COLOR_WHITE, rich ? 236 : -1);
            define (SERIES1, rich ? 75 : COLOR_CYAN, -1);
            define (SERIES2, rich ? 214 : COLOR_YELLOW, -1);
            define (SERIES3, rich ? 114 : COLOR_GREEN, -1);
            define (SERIES4, rich ? 176 : COLOR_MAGENTA, -1);
        }
    else
        {
            for (auto &a : role_attr)
                a = A_NORMAL;
            role_attr[TITLE] = role_attr[ACCENT] = role_attr[BAD] = A_BOLD;
            role_attr[DIM] = A_DIM;
            role_attr[TAB_ON] = A_REVERSE;
        }
}

void
shutdown ()
{
    endwin ();
}

int
width (std::string_view utf8)
{
    int w = 0;
    for (wchar_t wc : widen (utf8))
        w += wc_width (wc);
    return w;
}

std::string
fit (std::string_view utf8, int cols)
{
    if (cols <= 0)
        return {};
    std::wstring ws = widen (utf8);
    int total = 0;
    for (wchar_t wc : ws)
        total += wc_width (wc);
    if (total <= cols)
        return std::string (utf8);
    std::wstring out;
    int w = 0;
    for (wchar_t wc : ws)
        {
            int cw = wc_width (wc);
            if (w + cw > cols - 1)
                break;
            out += wc;
            w += cw;
        }
    out += L'…';
    return narrow (out);
}

std::string
pad (std::string_view utf8, int cols, char align)
{
    std::string s = fit (utf8, cols);
    int w = width (s);
    std::string fill (static_cast<std::size_t> (std::max (0, cols - w)), ' ');
    return align == 'r' ? fill + s : s + fill;
}

int
put (int y, int x, std::string_view utf8, attr_t a, int maxcols)
{
    if (y < 0 || y >= LINES || x >= COLS)
        return 0;
    int limit = COLS - x;
    if (maxcols >= 0)
        limit = std::min (limit, maxcols);
    if (limit <= 0)
        return 0;
    std::wstring ws = widen (utf8);
    std::wstring clipped;
    int w = 0;
    for (wchar_t wc : ws)
        {
            if (wc == L'\n' || wc == L'\t')
                wc = L' ';
            int cw = wc_width (wc);
            if (w + cw > limit)
                break;
            clipped += wc;
            w += cw;
        }
    attr_set (a, PAIR_NUMBER (a), nullptr);
    mvaddnwstr (y, x, clipped.c_str (), static_cast<int> (clipped.size ()));
    attr_set (A_NORMAL, 0, nullptr);
    return w;
}

Rect
Rect::inset (int dy, int dx) const
{
    return {y + dy, x + dx, std::max (0, h - 2 * dy), std::max (0, w - 2 * dx)};
}

Rect
Rect::top (int n) const
{
    return {y, x, std::clamp (n, 0, h), w};
}

Rect
Rect::below (int n) const
{
    n = std::clamp (n, 0, h);
    return {y + n, x, h - n, w};
}

std::vector<Rect>
Rect::columns (int n, int gap) const
{
    std::vector<Rect> out;
    if (n <= 0)
        return out;
    int each = std::max (0, (w - gap * (n - 1)) / n);
    int cx = x;
    for (int i = 0; i < n; ++i)
        {
            int cw = i == n - 1 ? std::max (0, right () - cx) : each;
            out.push_back ({y, cx, h, cw});
            cx += cw + gap;
        }
    return out;
}

std::vector<Rect>
Rect::rows (int n, int gap) const
{
    std::vector<Rect> out;
    if (n <= 0)
        return out;
    int each = std::max (0, (h - gap * (n - 1)) / n);
    int cy = y;
    for (int i = 0; i < n; ++i)
        {
            int ch = i == n - 1 ? std::max (0, bottom () - cy) : each;
            out.push_back ({cy, x, ch, w});
            cy += ch + gap;
        }
    return out;
}

Rect
frame (const Rect &r, std::string_view title, attr_t title_attr)
{
    if (r.h < 2 || r.w < 2)
        return {r.y, r.x, 0, 0};
    attr_t fa = attr (FRAME);
    int last_y = r.bottom () - 1, last_x = r.right () - 1;
    for (int i = r.x + 1; i < last_x; ++i)
        {
            put (r.y, i, "─", fa);
            put (last_y, i, "─", fa);
        }
    for (int j = r.y + 1; j < last_y; ++j)
        {
            put (j, r.x, "│", fa);
            put (j, last_x, "│", fa);
        }
    put (r.y, r.x, "╭", fa);
    put (r.y, last_x, "╮", fa);
    put (last_y, r.x, "╰", fa);
    put (last_y, last_x, "╯", fa);
    if (!title.empty () && r.w > 6)
        {
            std::string t = " " + fit (title, r.w - 6) + " ";
            put (r.y, r.x + 2, t, title_attr ? title_attr : attr (TITLE));
        }
    return r.inset (1, 1);
}

void
gauge (int y, int x, int w, double frac, attr_t fill)
{
    static const char *const eighths[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
    if (w <= 0)
        return;
    frac = std::clamp (frac, 0.0, 1.0);
    int units = static_cast<int> (std::lround (frac * w * 8));
    int full = units / 8, rem = units % 8;
    std::string bar;
    for (int i = 0; i < full; ++i)
        bar += "█";
    if (rem && full < w)
        bar += eighths[rem];
    int drawn = put (y, x, bar, fill, w);
    std::string rest;
    for (int i = drawn; i < w; ++i)
        rest += "·";
    put (y, x + drawn, rest, attr (DIM), w - drawn);
}

Braille::Braille (int cols, int rows)
    : cols_ (std::max (0, cols)), rows_ (std::max (0, rows)),
      bits_ (static_cast<std::size_t> (cols_ * rows_), 0)
{
}

void
Braille::clear ()
{
    std::ranges::fill (bits_, 0);
}

void
Braille::set (int x, int y)
{
    if (x < 0 || y < 0 || x >= dots_w () || y >= dots_h ())
        return;

    static constexpr std::uint8_t map[4][2] =
    {
        {0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}
    };
    bits_[static_cast<std::size_t> ((y / 4) * cols_ + x / 2)]
    |= map[y % 4][x % 2];
}

void
Braille::line (int x0, int y0, int x1, int y1)
{
    int dx = std::abs (x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs (y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;)
        {
            set (x0, y0);
            if (x0 == x1 && y0 == y1)
                break;
            int e2 = 2 * err;
            if (e2 >= dy)
                {
                    err += dy;
                    x0 += sx;
                }
            if (e2 <= dx)
                {
                    err += dx;
                    y0 += sy;
                }
        }
}

bool
Braille::used (int col, int row) const
{
    if (col < 0 || row < 0 || col >= cols_ || row >= rows_)
        return false;
    return bits_[static_cast<std::size_t> (row * cols_ + col)] != 0;
}

void
Braille::draw (int y, int x, attr_t a) const
{
    for (int r = 0; r < rows_; ++r)
        for (int c = 0; c < cols_; ++c)
            if (std::uint8_t b = bits_[static_cast<std::size_t> (r * cols_ + c)])
                {
                    wchar_t wc[2] = {static_cast<wchar_t> (0x2800 + b), 0};
                    attr_set (a, PAIR_NUMBER (a), nullptr);
                    mvaddnwstr (y + r, x + c, wc, 1);
                }
    attr_set (A_NORMAL, 0, nullptr);
}
}
