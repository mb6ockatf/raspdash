/* screen.hpp -- one page of the dashboard.

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

#include <string>
#include <vector>

#include "source.hpp"
#include "tui.hpp"
#include "util.hpp"

class Screen
{
public:
    virtual ~Screen () = default;

    virtual std::string title () const = 0;

    virtual void draw (const tui::Rect &area, util::TimePoint now) = 0;

    virtual bool key (int ch)
    {
        (void) ch;
        return false;
    }

    virtual std::vector<Source *> sources () const = 0;

    virtual std::vector<Source *> footer_sources () const
    {
        return sources ();
    }
};
