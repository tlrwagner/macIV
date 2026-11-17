// steam_achievements.inc - Steam achievements for macIV.
//
// GTA IV v1.0.8.0 reports achievements to Games for Windows - LIVE through
// xlive.dll ordinal #5278, XUserWriteAchievements(count, XUSER_ACHIEVEMENT*,
// XOVERLAPPED*). The runtime's xlive stand-in answers that call and drops
// it. This file registers its own handler for the same export (the game
// binary's static initialisers run after the runtime dylibs', so this one
// wins), forwards each achievement to Steam, then answers the game exactly
// as the stand-in did.
//
// The game's achievement ids are one numbering for GTA IV and both
// episodes, read from the game's own scripts (the id passed to the
// achievement native in each award site):
//   1-40   GTA IV, Rockstar's ACHIEVEMENT_IDS enum; the Complete Edition's
//          Steam API names are those same enum names
//   41-50  GFWL multiplayer achievements, not on Steam
//   51-55  The Lost and Damned (E1*), 56-65 The Ballad of Gay Tony (E2*);
//          API names from the Complete Edition's GTAIV.exe, matched to ids
//          by the script that awards each one
// The episodes also award some GTA IV ids (darts, pool, QUB3D...), so the
// table applies whichever game is running. An override line (see
// kOverrideFile below) can remap or disable any id.
//
// Steam is reached through libsteam_api.dylib (Steamworks flat API),
// loaded with dlopen from the app's Frameworks folder, so a missing
// library or a Steam client that is not running only turns the feature
// off. The Steam app id is GTA IV: The Complete Edition (12210); the
// account must own it.
//
// Options (environment, set by the launcher or by hand):
//   GTAIV_STEAM_ACHIEVEMENTS  "0" turns the bridge off (default on).
//
// Log: ~/Library/Logs/macIV-steam-achievements.log

#include <dlfcn.h>
#include <mach-o/dyld.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/runtime.h"
#include "option.h"

namespace maciv_steam {

constexpr const char* kSteamAppId = "12210";
// Which game is running: 0 GTA IV, 1 The Lost and Damned, 2 The Ballad of
// Gay Tony (see src/episodes.cpp).
constexpr recomp::gaddr kRunningEpisode = 0x0112D754;
// Optional overrides, one per line: "<episode> <id> <STEAM_API_NAME>", or
// "<episode> <id> -" to send nothing for that id, where episode is the game
// running when the id is awarded. '#' starts a comment.
constexpr const char* kOverrideFile = "steam_achievements.txt";

// Steam API name by game achievement id. Empty = not on Steam.
const char* const kSteamNames[] = {
    "",                          //  0
    "THETOUR",                   //  1 Off The Boat
    "SCOREDARTGAME",             //  2 One Hundred And Eighty
    "POOLWIN",                   //  3 Pool Shark
    "QUBBEAT",                   //  4 King Of QUB3D
    "MELEECOUNTER",              //  5 Finish Him
    "STREETRACE",                //  6 Genetically Superior
    "BIKEWHEELIE",               //  7 Wheelie Rider
    "TURKEY",                    //  8 Gobble Gobble
    "SATAXI",                    //  9 Driving Mr. Bellic
    "CARROLLS",                  // 10 Rolled Over
    "LOSERATING",                // 11 Walk Free
    "DRUGDONE",                  // 12 Courier Service
    "SAGUNCAR",                  // 13 Retail Therapy
    "CHAINREACTION",             // 14 Chain Reaction
    "WANTEDLEVEL6STARSURVIVAL",  // 15 One Man Army
    "KEYMISSION1",               // 16 Lowest Point
    "IMPORTDONE",                // 17 Order Fulfilled
    "WANTEDDONE",                // 18 Manhunt
    "VIGILDONE",                 // 19 Cleaned The Mean Streets
    "KEYMISSION0",               // 20 Fed The Fish
    "TAXIPASSANGER",             // 21 It'll Cost Ya
    "ALLHELITOURTAKEN",          // 22 Sightseer
    "DATING",                    // 23 Warm Coffee
    "SAHELI",                    // 24 That's How We Roll!
    "HALFMILLION",               // 25 Half Million
    "KEYMISSION2",               // 26 Impossible Trinity
    "OPENEDALDERNEY",            // 27 Full Exploration
    "SMSTHEFTDONE",              // 28 You Got The Message
    "USJDONE",                   // 29 Dare Devil
    "ASSASSIN",                  // 30 Assassin's Greed
    "HIDDENPACKAGES",            // 31 Endangered Species
    "UNDERALLBRIDGES",           // 32 Under The Radar
    "SABOMBCAR",                 // 33 Dial B For Bomb
    "KEYMISSION3",               // 34 Gracefully Taken
    "FRIENDSLIKEYOU",            // 35 Liberty City (5)
    "METRANDOM",                 // 36 No More Strangers
    "KEYMISSION4",               // 37 That Special Someone
    "STORYDONE",                 // 38 You Won!
    "STREETLIGHTSDOWN",          // 39 Liberty City Minute
    "GAMECOMPLETION100",         // 40 Key To The City
    "", "", "", "", "", "", "", "", "", "",  // 41-50 GFWL multiplayer
    "E1INTRO",                   // 51 TLAD: One Percenter       (billy1)
    "E1BECOMELEADER",            // 52 TLAD: The Lost Boy        (billy6)
    "E1FINISHSTORY",             // 53 TLAD: Easy Rider          (e1endcredits)
    "E1KNOCKOFF",                // 54 TLAD: Get Good Wood       (bike races)
    "E1BUDDYSTAT",               // 55 TLAD: Full Chat           (Terry & Clay)
    "E2BASEJUMP",                // 56 TBoGT: Gone Down          (basejumping)
    "E2TRINITY",                 // 57 TBoGT: Diamonds Forever   (tony8)
    "E2GOLF",                    // 58 TBoGT: Four Play          (golf)
    "E2FIGHTC",                  // 59 TBoGT: Bear Fight         (fightclubring)
    "E2DANCE",                   // 60 TBoGT: Catch the Bus      (busstop)
    "E2DRUGS",                   // 61 TBoGT: Snow Queen         (drug wars)
    "E2FREEFALL",                // 62 TBoGT: Adrenaline Junkie  (parachute)
    "E2STORY",                   // 63 TBoGT: Maestro            (e2endcredits)
    "E2FAIRFLAIR",               // 64 TBoGT: Past the Velvet Rope (80% missions)
    "E2ALLFLAIR",                // 65 TBoGT: Gold Star          (100% missions)
};
constexpr uint32_t kSteamCount = sizeof(kSteamNames) / sizeof(kSteamNames[0]);
static_assert(kSteamCount == 66, "ids 0-65");

// ---- Steamworks flat API (resolved at run time) ----------------------------

using InitFlatFn = int (*)(char* err_msg_1024);  // ESteamAPIInitResult, 0 = OK
using InitFn = bool (*)();
using VoidFn = void (*)();
using BoolFn = bool (*)();
using UserStatsFn = void* (*)();
using SetAchFn = bool (*)(void*, const char*);
using GetAchFn = bool (*)(void*, const char*, bool*);
using StoreFn = bool (*)(void*);
using AttrFn = const char* (*)(void*, const char*, const char*);

struct Steam {
  void* lib = nullptr;
  void* stats = nullptr;
  VoidFn run_callbacks = nullptr;
  SetAchFn set = nullptr;
  GetAchFn get = nullptr;
  StoreFn store = nullptr;
  AttrFn attr = nullptr;
  bool ready = false;
};

std::mutex g_mu;
Steam g_steam;
bool g_tried = false;
std::map<std::pair<uint32_t, uint32_t>, std::string> g_overrides;  // {episode,id}
std::set<std::pair<uint32_t, uint32_t>> g_logged_unmapped;
// Unlocks Steam refused so far (its stats for the user may not have
// arrived yet): API name -> attempts left.
std::map<std::string, int> g_pending;

std::string HomeDir() {
  const char* h = std::getenv("HOME");
  return h ? h : "";
}

void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void Log(const char* fmt, ...) {
  char msg[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "macIV: steam: %s\n", msg);
  const std::string path = HomeDir() + "/Library/Logs/macIV-steam-achievements.log";
  if (FILE* f = std::fopen(path.c_str(), "a")) {
    char when[32];
    std::time_t t = std::time(nullptr);
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::fprintf(f, "[%s] %s\n", when, msg);
    std::fclose(f);
  }
}

std::string FrameworksDir() {
  char exe[PATH_MAX];
  uint32_t size = sizeof(exe);
  if (_NSGetExecutablePath(exe, &size) != 0) return "";
  std::string p(exe);
  const size_t slash = p.find_last_of('/');
  if (slash == std::string::npos) return "";
  return p.substr(0, slash) + "/../Frameworks";
}

void LoadOverrides() {
  const std::string path = recomp::DataDir() + "/" + kOverrideFile;
  std::ifstream in(path);
  if (!in) return;
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
    std::istringstream ss(line);
    uint32_t episode = 0, id = 0;
    std::string name;
    if (!(ss >> episode >> id >> name)) continue;
    g_overrides[{episode, id}] = name == "-" ? std::string() : name;
    ++n;
  }
  Log("loaded %d override(s) from %s", n, path.c_str());
}

template <typename T>
T Sym(void* lib, const char* name) {
  return reinterpret_cast<T>(dlsym(lib, name));
}

void RetryPending();

// Called with g_mu held.
bool EnsureSteam() {
  if (g_tried) return g_steam.ready;
  g_tried = true;
  LoadOverrides();

  // The app id SteamAPI_Init reads when there is no steam_appid.txt.
  setenv("SteamAppId", kSteamAppId, 0);
  setenv("SteamGameId", kSteamAppId, 0);

  const std::string fw = FrameworksDir();
  const std::string candidates[] = {
      fw + "/libsteam_api.dylib",
      "@executable_path/../Frameworks/libsteam_api.dylib",
      "libsteam_api.dylib",
  };
  for (const std::string& c : candidates) {
    if ((g_steam.lib = dlopen(c.c_str(), RTLD_NOW | RTLD_LOCAL))) break;
  }
  if (!g_steam.lib) {
    Log("libsteam_api.dylib not found; Steam achievements off (%s)", dlerror());
    return false;
  }

  auto is_running = Sym<BoolFn>(g_steam.lib, "SteamAPI_IsSteamRunning");
  if (is_running && !is_running()) {
    Log("the Steam client is not running; Steam achievements off for this session");
    return false;
  }

  bool ok = false;
  if (auto init_flat = Sym<InitFlatFn>(g_steam.lib, "SteamAPI_InitFlat")) {
    char err[1024] = {0};
    const int r = init_flat(err);
    ok = r == 0;
    if (!ok) Log("SteamAPI_InitFlat failed (%d): %s", r, err);
  } else if (auto init = Sym<InitFn>(g_steam.lib, "SteamAPI_Init")) {
    ok = init();
    if (!ok) Log("SteamAPI_Init failed");
  }
  if (!ok) {
    Log("is Steam signed in to an account that owns GTA IV: The Complete Edition (app %s)?",
        kSteamAppId);
    return false;
  }

  UserStatsFn user_stats = nullptr;
  for (const char* v : {"SteamAPI_SteamUserStats_v013", "SteamAPI_SteamUserStats_v012",
                        "SteamAPI_SteamUserStats_v011"}) {
    if ((user_stats = Sym<UserStatsFn>(g_steam.lib, v))) break;
  }
  g_steam.run_callbacks = Sym<VoidFn>(g_steam.lib, "SteamAPI_RunCallbacks");
  g_steam.set = Sym<SetAchFn>(g_steam.lib, "SteamAPI_ISteamUserStats_SetAchievement");
  g_steam.get = Sym<GetAchFn>(g_steam.lib, "SteamAPI_ISteamUserStats_GetAchievement");
  g_steam.store = Sym<StoreFn>(g_steam.lib, "SteamAPI_ISteamUserStats_StoreStats");
  g_steam.attr = Sym<AttrFn>(g_steam.lib, "SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute");
  g_steam.stats = user_stats ? user_stats() : nullptr;
  if (!g_steam.stats || !g_steam.set || !g_steam.store) {
    Log("this libsteam_api.dylib lacks the ISteamUserStats flat API; Steam achievements off");
    return false;
  }

  g_steam.ready = true;
  Log("connected to Steam as app %s", kSteamAppId);

  // Keep Steam's callback queue drained for the life of the game, and retry
  // unlocks Steam was not ready for.
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      std::lock_guard<std::mutex> lock(g_mu);
      if (g_steam.run_callbacks) g_steam.run_callbacks();
      RetryPending();
    }
  }).detach();
  return true;
}

// The Steam API name for a game achievement id, or "" for none.
std::string SteamName(uint32_t episode, uint32_t id) {
  if (auto it = g_overrides.find({episode, id}); it != g_overrides.end()) return it->second;
  if (id < kSteamCount) return kSteamNames[id];
  return "";
}

// Sets and stores one achievement; false if Steam refused it.
bool TrySet(const std::string& name) {
  if (!g_steam.set(g_steam.stats, name.c_str())) return false;
  const char* display = g_steam.attr ? g_steam.attr(g_steam.stats, name.c_str(), "name") : "";
  const bool stored = g_steam.store(g_steam.stats);
  Log("unlocked %s \"%s\"%s", name.c_str(), display ? display : "",
      stored ? "" : " (StoreStats failed; Steam retries it)");
  return true;
}

void Unlock(uint32_t episode, uint32_t id) {
  const std::string name = SteamName(episode, id);
  if (name.empty()) {
    if (g_logged_unmapped.insert({episode, id}).second)
      Log("game awarded achievement %u (episode %u): no Steam mapping, not sent. "
          "Add a line \"%u %u <API_NAME>\" to %s/%s to map it.",
          id, episode, episode, id, recomp::DataDir().c_str(), kOverrideFile);
    return;
  }
  bool already = false;
  if (g_steam.get && g_steam.get(g_steam.stats, name.c_str(), &already) && already) {
    return;  // the game re-awards on every load; nothing to do.
  }
  if (!TrySet(name)) {
    if (!g_pending.count(name)) {
      g_pending[name] = 120;  // two minutes of retries
      Log("Steam not ready for %s (id %u, episode %u); will retry", name.c_str(), id, episode);
    }
  }
}

void RetryPending() {
  for (auto it = g_pending.begin(); it != g_pending.end();) {
    if (TrySet(it->first)) {
      it = g_pending.erase(it);
    } else if (--it->second <= 0) {
      Log("Steam never accepted %s: not an achievement of app %s, or stats unavailable",
          it->first.c_str(), kSteamAppId);
      it = g_pending.erase(it);
    } else {
      ++it;
    }
  }
}

// ---- xlive.dll #5278 ----------------------------------------------------------

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorIoPending = 997;

// As the runtime's xlive stand-in completes a call: synchronously, or
// through the XOVERLAPPED when one is given.
uint32_t Finish(recomp::gaddr ov, uint32_t result) {
  using namespace recomp;
  if (!ov) return result;
  Wr32(ov + 0, result);
  Wr32(ov + 4, 0);
  Wr32(ov + 24, result);
  if (uint32_t ev = Rd32(ov + 12)) SignalEventHandle(ev, false);
  return kErrorIoPending;
}

// Connect shortly after launch, so Steam shows the game as running (and
// counts play time) before the first achievement.
struct EarlyConnect {
  EarlyConnect() {
    if (!OptionOn("GTAIV_STEAM_ACHIEVEMENTS", true)) return;
    std::thread([] {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      std::lock_guard<std::mutex> lock(g_mu);
      EnsureSteam();
    }).detach();
  }
} g_early_connect;

}  // namespace maciv_steam

namespace recomp::hle {

// XUserWriteAchievements(DWORD count, const XUSER_ACHIEVEMENT* list,
//                        XOVERLAPPED* ov); XUSER_ACHIEVEMENT is
// { DWORD dwUserIndex; DWORD dwAchievementId; }.
HLE_NAMED("xlive.dll", "#5278", MacIVSteamWriteAchievements, 3) {
  const uint32_t count = uint32_t(Arg(c, 0));
  const gaddr list = gaddr(Arg(c, 1));
  const gaddr ov = gaddr(Arg(c, 2));

  static const bool on = OptionOn("GTAIV_STEAM_ACHIEVEMENTS", true);
  if (on && list && count && count < 256) {
    std::lock_guard<std::mutex> lock(maciv_steam::g_mu);
    if (maciv_steam::EnsureSteam()) {
      const uint32_t episode = Rd32(maciv_steam::kRunningEpisode);
      for (uint32_t i = 0; i < count; ++i)
        maciv_steam::Unlock(episode, Rd32(list + 8 * i + 4));
    }
  }
  return maciv_steam::Finish(ov, maciv_steam::kErrorSuccess);
}

}  // namespace recomp::hle
