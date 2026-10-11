// ds1: Dungeon Siege, natively.
//
//   ds1 [--game-dir DIR] [-- GAME ARGS...]
//
// DIR is the game install (the folder with DungeonSiege.exe, Resources/,
// Maps/). Defaults to $DS1_GAME_DIR. Arguments after `--` go on the game's
// own command line (Dungeon Siege reads key=value settings there, e.g.
// fullscreen=false). Set DS1_TRACE=1 to log every HLE call.

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "runtime/log.h"
#include "runtime/runtime.h"

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
  if (const char* t = std::getenv("DS1_TRACE")) ds1::g_trace = *t && *t != '0';

  std::string exe = game_dir + "/DungeonSiege.exe";
  struct stat st;
  if (stat(exe.c_str(), &st) != 0) {
    std::fprintf(stderr, "ds1: %s not found\n", exe.c_str());
    return 2;
  }
  return ds1::RunProcess(exe, game_dir, game_args);
}
