// ds1: Dungeon Siege, natively.
//
//   ds1 [--game-dir DIR] [-- GAME ARGS...]
//
// DIR is the game install (the folder with DungeonSiege.exe, Resources/,
// Maps/). Defaults to $DS1_GAME_DIR. Arguments after `--` go on the game's
// own command line (Dungeon Siege reads key=value settings there, e.g.
// fullscreen=false). Set DS1_TRACE=1 to log every HLE call.

#include <sys/stat.h>

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "runtime/log.h"
#include "runtime/runtime.h"

namespace {

// Dungeon Siege's input code (FUN_00414642) has two modes. Windowed, its
// cursor is the pointer position. Fullscreen (the only mode this build
// allows: the `fullscreen` setting's store is NOPed out at 0x41653A) it
// re-centres the pointer every frame and moves its own cursor by the
// scaled delta (see SyncedCursorPos in x86recomp's user32_window.cpp).
// The update that moves the game's cursor first re-centres (ref = centre,
// SetCursorPos(centre)) and then reads; other reads only peek into locals.
// The fields are in the game's AppModule, which it keeps in the window's
// GWL_USERDATA:
//   +0x12C/+0x130 cursor (client px),
//   +0x17C/+0x180 sensitivity (+0x188/+0x18C when flags +0x48 bit 28),
//   +0x74 fullscreen flag (the scale applies when set or with bit 28).
bool GameCursor(uint32_t app, int32_t* x, int32_t* y, float* kx, float* ky) {
  using namespace recomp;
  if (!g_proc->mem.IsCommitted(app) || !g_proc->mem.IsCommitted(app + 0x190))
    return false;
  *x = int32_t(Rd32(app + 0x12C));
  *y = int32_t(Rd32(app + 0x130));
  bool alt = Rd32(app + 0x48) >> 28 & 1;
  *kx = *ky = 1;
  if (GPtr(app + 0x74)[0] || alt) {
    *kx = std::bit_cast<float>(Rd32(app + (alt ? 0x188 : 0x17C)));
    *ky = std::bit_cast<float>(Rd32(app + (alt ? 0x18C : 0x180)));
    if (!(*kx > 0.01f && *kx < 100.0f)) *kx = 1;
    if (!(*ky > 0.01f && *ky < 100.0f)) *ky = 1;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string game_dir, game_args;
  if (const char* e = std::getenv("DS1_GAME_DIR")) game_dir = e;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--game-dir") && i + 1 < argc) {
      game_dir = argv[++i];
    } else if (!std::strcmp(argv[i], "--")) {
      for (i++; i < argc; i++) {
        if (!game_args.empty()) game_args += ' ';
        game_args += argv[i];
      }
    } else {
      std::fprintf(stderr, "usage: %s [--game-dir DIR] [-- GAME ARGS...]\n", argv[0]);
      return 2;
    }
  }
  if (game_dir.empty()) {
    std::fprintf(stderr, "ds1: pass --game-dir or set DS1_GAME_DIR\n");
    return 2;
  }
  while (game_dir.size() > 1 && game_dir.back() == '/') game_dir.pop_back();
  if (const char* t = std::getenv("DS1_TRACE")) recomp::g_trace = *t && *t != '0';

  std::string exe = game_dir + "/DungeonSiege.exe";
  struct stat st;
  if (stat(exe.c_str(), &st) != 0) {
    std::fprintf(stderr, "ds1: %s not found\n", exe.c_str());
    return 2;
  }

  recomp::GameConfig game;
  game.id = "ds1";
  game.guest_dir = "C:\\Dungeon Siege";
  game.exe_name = "DungeonSiege.exe";
  // What the retail installer writes (the GOG install writes nothing).
  const std::string key = "HKLM\\Software\\Microsoft\\Microsoft Games\\DungeonSiege\\1.0";
  game.registry = {{key, "EXE Path", game.guest_dir},
                   {key, "InstallationDirectory", game.guest_dir}};
  game.game_cursor = GameCursor;
  return recomp::RunProcess(game, exe, game_dir, game_args);
}
