// binkw32 (Bink video). M3 bring-up: BinkOpen fails, so the game skips
// its movies the way it would for a missing file. Playback through
// ffmpeg's Bink decoder is M4. Functions that only make sense on an open
// movie are fatal if reached, so nothing silently pretends to play.

#include "runtime/hle/hle.h"
#include "runtime/log.h"

namespace ds1::hle {
namespace {

HLE_NAMED("binkw32.dll", "_BinkSetSoundSystem@8", BinkSetSoundSystem, 2) { return 1; }
HLE_NAMED("binkw32.dll", "_BinkSetSoundTrack@4", BinkSetSoundTrack, 1) { return 0; }
HLE_NAMED("binkw32.dll", "_BinkOpenMiles@4", BinkOpenMiles, 1) { return 0; }

HLE_NAMED("binkw32.dll", "_BinkOpen@8", BinkOpen, 2) {
  WarnF("binkw32: BinkOpen(%s): video playback not implemented yet (M4), "
        "skipping\n", GStr(Arg(c, 0)).c_str());
  return 0;
}

}  // namespace
}  // namespace ds1::hle
