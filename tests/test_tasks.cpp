/* test_tasks.cpp -- Google Tasks parsing, ordering and OAuth helpers.

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

#include <filesystem>
#include <unistd.h>
#include <sys/stat.h>

#include "check.hpp"
#include "tasks.hpp"

using namespace std::chrono;

int
main ()
{
    return check::run ([]
    {

        CHECK (tasks::base64url ("") == "");
        CHECK (tasks::base64url ("f") == "Zg");
        CHECK (tasks::base64url ("fo") == "Zm8");
        CHECK (tasks::base64url ("foo") == "Zm9v");
        CHECK (tasks::base64url ("foob") == "Zm9vYg");
        CHECK (tasks::base64url ("fooba") == "Zm9vYmE");
        CHECK (tasks::base64url ("foobar") == "Zm9vYmFy");
        CHECK (tasks::base64url ("\xfb\xff") == "-_8");

        CHECK (tasks::pkce_challenge ("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")
               == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");

        std::string r1 = tasks::random_token (48), r2 = tasks::random_token (48);
        CHECK (r1.size () == 64 && r1 != r2);

        std::string next;
        auto p1 = tasks::parse_lists (check::data ("tasks-lists-page1.json"), next);
        CHECK (p1.size () == 2);
        CHECK (next == "CgwI3oSfuQYQ-I2nkQE");
        CHECK (p1[1].title == "Учёба");
        auto p2 = tasks::parse_lists (check::data ("tasks-lists-page2.json"), next);
        CHECK (p2.size () == 1 && next.empty ());

        std::vector<tasks::ListRef> all = p1;
        all.insert (all.end (), p2.begin (), p2.end ());
        std::vector<std::string> missing;
        auto chosen = tasks::select_lists (all, {"Shopping", "Nope", "My Tasks"},
        missing);
        CHECK (chosen.size () == 2);
        CHECK (chosen[0].title == "Shopping" && chosen[1].title == "My Tasks");
        CHECK (missing.size () == 1 && missing[0] == "Nope");
        missing.clear ();
        CHECK (tasks::select_lists (all, {}, missing).size () == 3);
        CHECK (missing.empty ());

        auto flat = tasks::parse_tasks (check::data ("tasks-items.json"), next);
        CHECK (flat.size () == 5);
        CHECK (next == "CiAKGjBpNDd2Nmp2Zml2cXRwYjBpOXA");
        auto ordered = tasks::order_tasks (flat);
        CHECK (ordered.size () == 5);
        CHECK (ordered[0].title == "Prepare for probability theory test");
        CHECK (ordered[0].depth == 0);
        CHECK (ordered[1].title == "Solve problem set 2"
               && ordered[1].depth == 1 && ordered[1].completed);
        CHECK (ordered[2].title == "Read chapter 3" && ordered[2].depth == 1);
        CHECK (ordered[3].title == "Курсовой проект: языки программирования"
               && ordered[3].depth == 0);
        CHECK (ordered[3].due == 2026y / October / 7);
        CHECK (ordered[3].notes.starts_with ("Отчёт и презентация\n"));
        CHECK (ordered[4].title == "Orphan subtask" && ordered[4].depth == 0);
        CHECK (!ordered[1].due);

        tasks::Snapshot snap;
        snap.lists.push_back ({"L1", "Учёба", ordered});
        snap.missing = {"Archive"};
        std::string sel;
        auto back = tasks::snapshot_from_json (
                        tasks::snapshot_to_json (snap, "lists=Учёба"), sel);
        CHECK (sel == "lists=Учёба");
        CHECK (back.missing == snap.missing);
        CHECK (back.lists.size () == 1 && back.lists[0].title == "Учёба");
        CHECK (back.lists[0].tasks.size () == ordered.size ());
        CHECK (back.lists[0].tasks[1].depth == 1
               && back.lists[0].tasks[1].completed);
        CHECK (back.lists[0].tasks[3].due == 2026y / October / 7);
        CHECK (!back.lists[0].tasks[1].due);

        std::vector<tasks::Task> cyc (2);
        cyc[0].id = "a";
        cyc[0].parent = "b";
        cyc[1].id = "b";
        cyc[1].parent = "a";
        CHECK (tasks::order_tasks (cyc).size () == 2);

        auto cs = tasks::parse_client_secret (check::data ("tasks-client-secret.json"));
        CHECK (cs.client_id.ends_with (".apps.googleusercontent.com"));
        CHECK (cs.client_secret == "test-client-secret");
        CHECK (cs.token_uri == "https://oauth2.googleapis.com/token");
        bool threw = false;
        try
            {
                tasks::parse_client_secret (R"({"web":{"client_id":"x"}})");
            }
        catch (const std::runtime_error &)
            {
                threw = true;
            }
        CHECK (threw);

        auto now = sys_days{2026y / October / 5} + 12h;
        auto tok = tasks::parse_token_response (
                       check::data ("tasks-token-response.json"), now);
        CHECK (tok.access_token == "ya29.a0AfakeAccessToken");
        CHECK (tok.refresh_token == "1//0gFAKErefreshTOKEN");
        CHECK (tok.expires_at == now + 3599s);

        std::string code;
        try
            {
                tasks::parse_token_response (check::data ("tasks-token-error.json"),
                                             now);
            }
        catch (const tasks::OAuthError &e)
            {
                code = e.code ();
            }
        CHECK (code == "invalid_grant");

        namespace fs = std::filesystem;
        fs::path dir = fs::temp_directory_path ()
                       / std::format ("raspdash-test-{}", ::getpid ());
        fs::path file = dir / "sub" / "token.json";
        tasks::write_token_file (file, tok);
        CHECK (tasks::parse_token_file (tasks::read_file (file))
               == "1//0gFAKErefreshTOKEN");
        struct stat st;
        CHECK (::stat (file.c_str (), &st) == 0 && (st.st_mode & 0777) == 0600);
        tasks::write_token_file (file, tok);
        CHECK (fs::exists (file) && !fs::exists (file.string () + ".tmp"));
        fs::remove_all (dir);
    });
}
