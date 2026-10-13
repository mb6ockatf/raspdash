/* tui.hpp -- drawing helpers over ncursesw.

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

#pragma once

#ifndef NCURSES_WIDECHAR
#define NCURSES_WIDECHAR 1
#endif
#include <curses.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tui
{

enum Role : short
{
    NORMAL = 1,
    DIM,
    TITLE,
    ACCENT,
    GOOD,
    WARN,
    BAD,
    FRAME,
    TAB_ON,
    TAB_OFF,
    SERIES1,
    SERIES2,
    SERIES3,
    SERIES4,
    PLOT_YELLOW,
    PLOT_GREEN,
    PLOT_BLUE,
    PLOT_CYAN,
    PLOT_MAGENTA,
    PLOT_RED,
    PLOT_ORANGE,
    PLOT_WHITE,
    ROLE_COUNT
};

attr_t attr (Role role);

Role level (double frac, double warn = 0.7, double bad = 0.9);

void set_nerd_icons (bool on);
bool nerd_icons ();

const char *icon (const char *nerd, const char *fallback);

constexpr int KEY_CLICK = KEY_MAX + 1;

void init ();
void shutdown ();

int read_key ();

int width (std::string_view utf8);

std::string fit (std::string_view utf8, int cols);

std::string pad (std::string_view utf8, int cols, char align = 'l');

int put (int y, int x, std::string_view utf8, attr_t a = A_NORMAL,
         int maxcols = -1);

struct Rect
{
    int y = 0, x = 0, h = 0, w = 0;

    bool empty () const
    {
        return h <= 0 || w <= 0;
    }
    int bottom () const
    {
        return y + h;
    }
    int right () const
    {
        return x + w;
    }

    Rect inset (int dy, int dx) const;

    Rect top (int n) const;
    Rect below (int n) const;

    std::vector<Rect> columns (int n, int gap = 1) const;
    std::vector<Rect> rows (int n, int gap = 0) const;
};

Rect frame (const Rect &r, std::string_view title = {},
            attr_t title_attr = 0);

void gauge (int y, int x, int w, double frac, attr_t fill);

class Braille
{
public:
    Braille (int cols, int rows);

    int cols () const
    {
        return cols_;
    }
    int rows () const
    {
        return rows_;
    }
    int dots_w () const
    {
        return cols_ * 2;
    }
    int dots_h () const
    {
        return rows_ * 4;
    }

    void set (int x, int y);
    void line (int x0, int y0, int x1, int y1);
    void clear ();

    void draw (int y, int x, attr_t a) const;

    bool used (int col, int row) const;

private:
    int cols_, rows_;
    std::vector<std::uint8_t> bits_;
};
}
