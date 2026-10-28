// SPDX-License-Identifier: ISC
#include "shinki_emu.h"
#include "../impacto.h"
#include "../game.h"
#include "../log.h"
#include "../inputsystem.h"
#include "../workqueue.h"
#include "../userconfig.h"
#include "../profile/profile.h"
#include "../profile/profile_internal.h"
#include "../profile/basepaths.h"
#include "../profile/gamedefinitions.h"
#include "../profile/game.h"
#include "../profile/sprites.h"
#include "../profile/fonts.h"
#include "../profile/animations.h"
#include "../audio/audiosystem.h"
#include "../video/videosystem.h"
#include "../renderer/opengl/window.h"
#include "../renderer/opengl/glc.h"
#include "../text/dialoguepage.h"
#include "../ui/ui.h"
#include "../background2d.h"
#include "../character2d.h"
#include "../effects/blur.h"
#include "../effects/mosaic.h"
#include "../effects/wave.h"
#include "../mask2d.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <utf8cpp/utf8.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#include <optional>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using namespace Impacto;
namespace fs = std::filesystem;
ShinkiEmuBytes Bytes(std::string_view s) {
  return {reinterpret_cast<uint8_t const*>(s.data()), s.size()};
}
std::string Text(ShinkiEmuBytes b, size_t max = ShinkiEmuMAX_CONFIG_TEXT) {
  if (b.len > max || (b.len && !b.data))
    throw std::invalid_argument("Invalid text span");
  std::string s(b.len ? reinterpret_cast<char const*>(b.data) : "", b.len);
  if (s.find('\0') != std::string::npos)
    throw std::invalid_argument("Embedded NUL in text");
  if (!utf8::is_valid(s.begin(), s.end()))
    throw std::invalid_argument("Invalid UTF-8 text");
  return s;
}
size_t Utf8Prefix(std::string_view text, size_t capacity) noexcept {
  size_t size = std::min(text.size(), capacity);
  if (size < text.size())
    while (size && (static_cast<unsigned char>(text[size]) & 0xc0) == 0x80)
      --size;
  return size;
}
ShinkiEmuStatus Error(ShinkiEmuError* e, ShinkiEmuStatus status,
                     std::string_view code, std::string_view message) noexcept {
  if (e) {
    *e = {};
    e->code_len = static_cast<uint32_t>(Utf8Prefix(code, sizeof(e->code)));
    e->message_len =
        static_cast<uint32_t>(Utf8Prefix(message, sizeof(e->message)));
    std::memcpy(e->code, code.data(), e->code_len);
    std::memcpy(e->message, message.data(), e->message_len);
  }
  return status;
}
template <class F>
ShinkiEmuStatus Guard(ShinkiEmuError* e, F&& f) noexcept {
  if (e) *e = {};
  try {
    return f();
  } catch (std::invalid_argument const& ex) {
    return Error(e, ShinkiEmuINVALID_ARGUMENT, "impacto.argument", ex.what());
  } catch (std::exception const& ex) {
    return Error(e, ShinkiEmuCORE_ERROR, "impacto.native", ex.what());
  } catch (...) {
    return Error(e, ShinkiEmuPANICKED, "impacto.exception",
                 "Unknown native exception");
  }
}
struct Cancelled {};
struct Settings {
  fs::path assets, resources, movie_order;
  bool backups = true;
};
struct Frame {
  std::atomic<size_t> references{1};
  std::vector<uint8_t> pixels = std::vector<uint8_t>(1920 * 1080 * 4);
  uint64_t generation = 0, timestamp = 0;
  uint32_t width = 1920, height = 1080;
};
void RetainFrame(Frame* frame) noexcept {
  frame->references.fetch_add(1, std::memory_order_relaxed);
}
void DropFrame(Frame* frame) noexcept {
  if (frame && frame->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
    delete frame;
}
class FrameRef {
 public:
  FrameRef() = default;
  explicit FrameRef(Frame* frame) noexcept : frame_(frame) {}
  ~FrameRef() { DropFrame(frame_); }
  FrameRef(FrameRef const&) = delete;
  FrameRef& operator=(FrameRef const&) = delete;
  FrameRef(FrameRef&& other) noexcept
      : frame_(std::exchange(other.frame_, nullptr)) {}
  FrameRef& operator=(FrameRef&& other) noexcept {
    if (this != &other) {
      DropFrame(frame_);
      frame_ = std::exchange(other.frame_, nullptr);
    }
    return *this;
  }
  Frame* get() const noexcept { return frame_; }

 private:
  Frame* frame_ = nullptr;
};
struct Session {
  ~Session();
  Settings settings;
  ShinkiEmuHostServices host{};
  uint64_t epoch = 0;
  std::atomic<bool> cancelled{false}, paused{false}, suspended{false};
  std::thread worker;
  std::mutex mutex;
  std::condition_variable ready;
  bool initialized = false, done = false;
  uint32_t status = ShinkiEmuSESSION_WAITING;
  ShinkiEmuError failure{};
  Frame* frame = nullptr;
  // Pool ownership and independent CPU leases share only Frame's intrusive
  // count.
  std::array<FrameRef, 4> frames;
  std::vector<SDL_Event> events;
  std::vector<SDL_Event> input_scratch;
};
std::mutex GlobalMutex;
Session* Active = nullptr;
Session::~Session() {
  cancelled = true;
  if (worker.joinable()) worker.join();
  std::lock_guard lock(GlobalMutex);
  if (Active == this) Active = nullptr;
}
thread_local Session* NativeSession = nullptr;
thread_local bool Closing = false;
ShinkiEmuDiagnostics Diagnostics{};
void Emit(uint32_t level, std::string_view message) noexcept {
  if (!Diagnostics.emit) return;
  ShinkiEmuLogRecord record{};
  record.level = level;
  record.target = Bytes("impacto");
  record.message = Bytes(message);
  Diagnostics.emit(Diagnostics.context, &record);
}
void SDLCALL NativeLog(void*, int, SDL_LogPriority priority,
                       char const* message) {
  uint32_t level = priority >= SDL_LOG_PRIORITY_ERROR  ? ShinkiEmuLOG_ERROR
                   : priority == SDL_LOG_PRIORITY_WARN ? ShinkiEmuLOG_WARN
                   : priority == SDL_LOG_PRIORITY_INFO ? ShinkiEmuLOG_INFO
                                                       : ShinkiEmuLOG_DEBUG;
  Emit(level, message ? message : "");
}
std::string LuaString(std::string_view s) {
  std::string result = "\"";
  for (unsigned char c : s) {
    if (c == '\\' || c == '"') {
      result += '\\';
      result += static_cast<char>(c);
    } else if (c < 32 || c >= 127) {
      result += '\\';
      result += static_cast<char>('0' + c / 100);
      result += static_cast<char>('0' + c / 10 % 10);
      result += static_cast<char>('0' + c % 10);
    } else
      result += static_cast<char>(c);
  }
  return result + '"';
}
void Lua(std::string const& code) {
  if (luaL_dostring(Profile::LuaState, code.c_str()))
    throw std::runtime_error(lua_tostring(Profile::LuaState, -1));
}
fs::path Absolute(std::string const& value) {
  if (value.empty())
    throw std::invalid_argument("A path configuration is required");
  return fs::weakly_canonical(fs::absolute(fs::u8path(value)));
}
bool Within(fs::path const& child, fs::path const& parent) {
  auto relative = child.lexically_relative(parent);
  return !relative.empty() && *relative.begin() != ".." &&
         !relative.is_absolute();
}
bool IsMpk(fs::path const& path) {
  std::array<unsigned char, 12> header{};
  std::ifstream stream(path, std::ios::binary);
  if (!stream.read(reinterpret_cast<char*>(header.data()), header.size()))
    return false;
  static constexpr unsigned char signature[] = {'M', 'P', 'K', 0, 0, 0, 2, 0};
  return std::equal(std::begin(signature), std::end(signature),
                    header.begin()) &&
         (header[8] || header[9] || header[10] || header[11]);
}
void ValidateAssets(Settings const& s) {
  for (auto name :
       {"script.mpk", "system.mpk", "bg1.mpk", "bg2.mpk", "voice.mpk",
        "bgm.mpk", "se.mpk", "sysse.mpk", "mask.mpk", "chara.mpk"}) {
    if (!IsMpk(s.assets / name))
      throw std::invalid_argument(
          std::string("Missing or unsupported native MPK v2 archive: ") + name);
  }
  if (s.backups && (!IsMpk(s.assets / "script_bak.mpk") ||
                    !IsMpk(s.assets / "system_bak.mpk")))
    throw std::invalid_argument(
        "use_backup_archives requires script_bak.mpk and system_bak.mpk");
  if (!fs::is_regular_file(s.resources / "profiles/cc/game.lua") ||
      !fs::is_regular_file(s.resources / "profiles/cc-steam/game.lua") ||
      !fs::is_directory(s.resources / "resources/cc") ||
      !fs::is_directory(s.resources / "src/shaders/opengl"))
    throw std::invalid_argument(
        "resources_root must contain profiles/cc, profiles/cc-steam, "
        "resources/cc and "
        "src/shaders/opengl");
}
ShinkiEmuSurfaceInfo Surface(uint32_t w = 1920, uint32_t h = 1080) {
  return {ShinkiEmuSURFACE_CPU,
          ShinkiEmuPIXEL_RGBA8,
          ShinkiEmuCOLOR_SRGB,
          ShinkiEmuALPHA_IGNORED,
          ShinkiEmuROWS_BOTTOM_UP,
          {1920, 1080},
          {w, h},
          static_cast<uint64_t>(w) * 4};
}
void ResetNativeObjects() {
  std::exception_ptr failure;
  auto clean = [&](auto&& operation) {
    try {
      operation();
    } catch (...) {
      if (!failure) failure = std::current_exception();
    }
  };
  // Complete pending synchronous native loads while their GL context and
  // destination objects still exist. A failing stage never skips later cleanup.
  clean([] { WorkQueue::StopWorkQueue(); });
  clean([] { Video::VideoShutdown(); });
  clean([] { Audio::AudioShutdown(); });
  DialoguePages.clear();
  KeyWaitIcon.reset();
  clean([] {
    std::set<UI::Menu*> menus;
    for (auto& [_, group] : UI::Menus)
      for (auto* menu : group) menus.insert(menu);
    for (auto* menu :
         {UI::SystemMenuPtr, UI::TitleMenuPtr, UI::TrophyMenuPtr,
          UI::HelpMenuPtr, static_cast<UI::Menu*>(UI::SaveMenuPtr),
          static_cast<UI::Menu*>(UI::SelectionMenuPtr),
          static_cast<UI::Menu*>(UI::BacklogMenuPtr),
          static_cast<UI::Menu*>(UI::TipsMenuPtr),
          static_cast<UI::Menu*>(UI::OptionsMenuPtr), UI::LibraryMenuPtr,
          UI::ClearListMenuPtr, UI::AlbumMenuPtr, UI::MusicMenuPtr,
          UI::MovieMenuPtr, UI::ActorsVoiceMenuPtr})
      if (menu) menus.insert(menu);
    for (auto* menu : menus) delete menu;
    UI::Menus.clear();
    UI::FocusedMenu = nullptr;
    UI::SystemMenuPtr = new UI::NullMenu();
    UI::TitleMenuPtr = new UI::NullMenu();
    UI::TrophyMenuPtr = new UI::NullMenu();
    UI::HelpMenuPtr = new UI::NullMenu();
    UI::SaveMenuPtr = new UI::SaveMenu();
    UI::SelectionMenuPtr = nullptr;
    UI::BacklogMenuPtr = nullptr;
    UI::TipsMenuPtr = nullptr;
    UI::OptionsMenuPtr = nullptr;
    UI::LibraryMenuPtr = UI::ClearListMenuPtr = UI::AlbumMenuPtr =
        UI::MusicMenuPtr = UI::MovieMenuPtr = UI::ActorsVoiceMenuPtr = nullptr;
  });
  UI::SysMesBox::MsgBoxes.clear();
  for (auto& bg : Backgrounds) clean([&] { bg.Unload(); });
  for (auto& cha : Characters2D) clean([&] { cha.Unload(); });
  for (auto& cha : SpeakerPortraits) clean([&] { cha.Unload(); });
  Backgrounds = {};
  Screencaptures = {};
  Framebuffers = {};
  MaskCapture = {};
  ShaderScreencapture = {};
  Backgrounds2D.clear();
  Background2D::BgEffShaderMap.clear();
  Background2D::BgEffTextureIdMap.clear();
  Characters2D = {};
  SpeakerPortraits = {};
  for (auto& script : Vm::ScriptBuffers) {
    free(script.data());
    script = {};
  }
  for (auto& script : Vm::MsbBuffers) {
    free(script.data());
    script = {};
  }
  for (auto& [_, font] : Profile::Fonts) delete font;
  Profile::Fonts.clear();
  Profile::SpriteSheets.clear();
  Profile::Sprites.clear();
  Profile::Animations.clear();
  Profile::FixedSpriteAnimations.clear();
  clean([] { Effects::Blur.Reset(); });
  clean([] { Effects::Mosaic.Reset(); });
  Effects::WaveBG.ClearWaves();
  Effects::WaveEFF.ClearWaves();
  Effects::WaveCHA.ClearWaves();
  if (Renderer) {
    for (auto& mask : Masks2D)
      clean([&] {
        mask.UnloadSync();
        mask = {};
      });
    clean([] { Renderer->Shutdown(); });
    delete Renderer;
    Renderer = nullptr;
  }
  BaseRenderer::SheetPathToId.clear();
  BaseRenderer::LookupTextureIdToTexture.clear();
  BaseRenderer::SurfToId.clear();
  ShutdownCursors();
  if (Window) {
    clean([] { Window->Shutdown(); });
    delete static_cast<OpenGL::GLWindow*>(Window);
    Window = nullptr;
  }
  clean([] { Io::VfsShutdown(); });
  if (Profile::LuaState) clean([] { Profile::ClearProfile(); });
  Profile::GameDefinitions.clear();
  Profile::Game::HasInit = false;
  Game::ShouldQuit = false;
  SkipModeEnabled = AutoModeEnabled = SyncAutoModeEnabled = false;
  SyncAutoTime = 0;
  std::fill(std::begin(Input::KeyboardButtonIsDown),
            std::end(Input::KeyboardButtonIsDown), false);
  std::fill(std::begin(Input::MouseButtonIsDown),
            std::end(Input::MouseButtonIsDown), false);
  std::fill(std::begin(Input::ControllerButtonIsDown),
            std::end(Input::ControllerButtonIsDown), false);
  std::fill(std::begin(Input::ControllerAxisValue),
            std::end(Input::ControllerAxisValue), 0.0f);
  Input::CurrentInputDevice = Input::Device::Mouse;
  Input::CurrentPinch.reset();
  Input::ClearTouch();
  if (failure) std::rethrow_exception(failure);
}
void MountMovieOrder(Session& s) {
  if (s.settings.movie_order.empty()) {
    Emit(ShinkiEmuLOG_WARN,
         "No movie_order supplied. Native movie requests fail explicitly; "
         "directory ordering is not a movie-ID provider.");
    return;
  }
  auto size = fs::file_size(s.settings.movie_order);
  if (!size || size > 1024 * 1024)
    throw std::invalid_argument(
        "movie_order must be a nonempty CLS file of at most 1 MiB");
  std::string content(static_cast<size_t>(size), '\0');
  std::ifstream file(s.settings.movie_order, std::ios::binary);
  if (!file.read(content.data(), static_cast<std::streamsize>(size)))
    throw std::runtime_error("Could not read movie_order");
  if (content.find('\0') != std::string::npos ||
      !utf8::is_valid(content.begin(), content.end()))
    throw std::invalid_argument(
        "movie_order must be UTF-8 without embedded NUL");
  size_t begin = 0;
  while (begin < content.size()) {
    auto end = content.find('\n', begin);
    auto line = content.substr(
        begin, end == std::string::npos ? content.size() - begin : end - begin);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && (line == "." || line == ".." ||
                          line.find_first_of("/\\\\:") != std::string::npos))
      throw std::invalid_argument(
          "movie_order contains a path; CLS entries must be filenames");
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  std::unique_ptr<void, decltype(&free)> data(malloc(content.size()), free);
  if (!data) throw std::bad_alloc();
  std::memcpy(data.get(), content.data(), content.size());
  // The virtual filename determines the native CLS base directory. No sidecar
  // is created beside the original media.
  auto name = (s.settings.assets / "movie/1920x1080.cls").generic_string();
  auto result =
      Io::VfsMountMemory("movie", name, data.release(),
                         static_cast<int64_t>(content.size()), true, false);
  if (result != IoError_OK)
    throw std::runtime_error("Native CLS movie mapping mount failed");
}
void InitializeNative(Session& s) {
  Shinki::CheckCancellation();
  SDL_SetLogOutputFunction(NativeLog, nullptr);
  SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);
  UserConfig::UserConfigPath.clear();
  UserConfig::SetActiveGame("cc");
  Profile::Init();
  auto r = s.settings.resources.generic_string(),
       a = s.settings.assets.generic_string();
  Lua("root.BasePaths={RootInstallDir=" + LuaString(r) + ",RootGamedataDir=" +
      LuaString(a) + ",RootProfilesDir=" + LuaString(r + "/profiles") +
      ",RootPatchesDir=\"\",RootSavesDir=" + LuaString(r + "/saves") + "}");
  lua_getglobal(Profile::LuaState, "root");
  Profile::BasePaths::Configure();
  lua_pop(Profile::LuaState, 1);
  Profile::GameDefinitions.clear();
  Profile::GameDefinitions["cc"] = {
      .GameProfile = r + "/profiles/cc-steam/game.lua", .Name = "CHAOS;CHILD"};
  UserConfig::Configure();
  // CPU frame ownership requires this backing extent, independent of Host size.
  UserConfig::CommonSettings.ResolutionWidth = 1920;
  UserConfig::CommonSettings.ResolutionHeight = 1080;
  UserConfig::ActiveGameSettings().ResolutionWidth = 1920;
  UserConfig::ActiveGameSettings().ResolutionHeight = 1080;
  UserConfig::ActiveGameSettings().Display = DisplayMode::Windowed;
  // The packaged CC profile has no patch mount or patch-profile definition.
  UserConfig::ActiveGameSettings().UsePatch = false;
  Shinki::CheckCancellation();
  Game::ShouldQuit = false;
  Game::Init();
  Shinki::CheckCancellation();
  Game::InitGameProfile();
  MountMovieOrder(s);
  Shinki::CheckCancellation();
  Emit(ShinkiEmuLOG_WARN,
       "CC upstream profile has no native save implementation; save/load "
       "acceptance is unavailable. Backup archives are not a patch "
       "compatibility guarantee.");
}
void Run(Session* s) noexcept {
  NativeSession = s;
  try {
    InitializeNative(*s);
    {
      std::lock_guard lock(s->mutex);
      s->initialized = true;
      s->status =
          s->suspended ? ShinkiEmuSESSION_PAUSED : ShinkiEmuSESSION_RUNNING;
    }
    s->ready.notify_all();
    auto last = std::chrono::steady_clock::now();
    std::vector<SDL_Event> events;
    bool wasPaused = false;
    while (!s->cancelled && !Game::ShouldQuit) {
      Shinki::CheckCancellation();
      bool paused = s->paused.load() || s->suspended.load();
      if (paused != wasPaused) {
        for (auto group : {Audio::ACG_BGM, Audio::ACG_SE, Audio::ACG_Voice,
                           Audio::ACG_Movie}) {
          if (paused)
            Audio::PauseChannelGroup(group);
          else
            Audio::ResumeChannelGroup(group);
        }
        for (auto* player : Video::Players)
          if (player) player->SetPaused(paused);
        wasPaused = paused;
      }
      {
        std::lock_guard lock(s->mutex);
        events.swap(s->events);
        s->status = paused ? ShinkiEmuSESSION_PAUSED : ShinkiEmuSESSION_RUNNING;
      }
      for (auto& event : events) {
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
          std::fill(std::begin(Input::KeyboardButtonIsDown),
                    std::end(Input::KeyboardButtonIsDown), false);
          std::fill(std::begin(Input::MouseButtonIsDown),
                    std::end(Input::MouseButtonIsDown), false);
        }
        if (!SDL_PushEvent(&event)) throw std::runtime_error(SDL_GetError());
      }
      events.clear();
      auto now = std::chrono::steady_clock::now();
      float dt =
          std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
      last = now;
      if (!paused) {
        Game::Update(dt);
        Game::Render();
      }
      std::this_thread::sleep_until(now + std::chrono::microseconds(16667));
    }
  } catch (Cancelled const&) {
  } catch (std::exception const& ex) {
    std::lock_guard lock(s->mutex);
    Error(&s->failure, ShinkiEmuCORE_ERROR, "impacto.native", ex.what());
    s->status = ShinkiEmuSESSION_FAILED;
    Emit(ShinkiEmuLOG_ERROR, ex.what());
  } catch (...) {
    std::lock_guard lock(s->mutex);
    Error(&s->failure, ShinkiEmuPANICKED, "impacto.exception",
          "Unknown native worker exception");
    s->status = ShinkiEmuSESSION_FAILED;
  }
  Closing = true;
  try {
    ResetNativeObjects();
  } catch (std::exception const& ex) {
    std::lock_guard lock(s->mutex);
    if (s->status != ShinkiEmuSESSION_FAILED)
      Error(&s->failure, ShinkiEmuCORE_ERROR, "impacto.cleanup", ex.what());
    s->status = ShinkiEmuSESSION_FAILED;
    Emit(ShinkiEmuLOG_ERROR, ex.what());
  } catch (...) {
    std::lock_guard lock(s->mutex);
    if (s->status != ShinkiEmuSESSION_FAILED)
      Error(&s->failure, ShinkiEmuPANICKED, "impacto.cleanup",
            "Native cleanup failed");
    s->status = ShinkiEmuSESSION_FAILED;
  }
  Closing = false;
  NativeSession = nullptr;
  {
    std::lock_guard lock(s->mutex);
    s->done = true;
    if (s->status != ShinkiEmuSESSION_FAILED)
      s->status = ShinkiEmuSESSION_FINISHED;
  }
  s->ready.notify_all();
}
ShinkiEmuConfigField Field(std::string_view id, std::string_view label,
                          uint32_t kind, std::string_view text = "",
                          uint32_t flags = 0) {
  ShinkiEmuConfigField f{};
  f.id = Bytes(id);
  f.label = Bytes(label);
  f.group = Bytes("impacto");
  f.default_value.kind = kind;
  f.default_value.text = Bytes(text);
  f.max_bytes = 4096;
  f.flags = flags;
  return f;
}
ShinkiEmuStatus Initialize(ShinkiEmuDiagnostics const* d,
                          ShinkiEmuError* e) noexcept {
  return Guard(e, [&]() -> ShinkiEmuStatus {
    if (!d || d->header.size != sizeof(*d) || d->header.version != 3 ||
        !d->emit)
      throw std::invalid_argument("Expected C ABI v3 diagnostic sink");
    std::lock_guard lock(GlobalMutex);
    if (Active)
      return Error(e, ShinkiEmuINVALID_STATE, "impacto.active",
                   "Cannot replace diagnostics during a session");
    Diagnostics = *d;
    return ShinkiEmuOK;
  });
}
ShinkiEmuStatus Describe(ShinkiEmuDescriptor* out, ShinkiEmuError* e) noexcept {
  return Guard(e, [&]() -> ShinkiEmuStatus {
    if (!out) throw std::invalid_argument("Null descriptor");
    static ShinkiEmuBytes choices[] = {Bytes("cc")};
    static ShinkiEmuBytes formats[] = {Bytes("mpk")};
    static auto fields = [] {
      std::array<ShinkiEmuConfigField, 5> f{
          Field("profile", "Native game profile", ShinkiEmuVALUE_ENUM, "cc"),
          Field("assets_root", "CC USRDIR (empty: game/USRDIR)",
                ShinkiEmuVALUE_STRING, "", ShinkiEmuCONFIG_FLAG_DIRECTORY_PATH),
          Field("resources_root", "Resource root (empty: core/Resources)",
                ShinkiEmuVALUE_STRING, "", ShinkiEmuCONFIG_FLAG_DIRECTORY_PATH),
          Field("use_backup_archives", "Use script_bak/system_bak archives",
                ShinkiEmuVALUE_BOOL),
          Field("movie_order", "Verified movie ID order (native CLS)",
                ShinkiEmuVALUE_STRING, "", ShinkiEmuCONFIG_FLAG_FILE_PATH)};
      f[0].choices = choices;
      f[0].choice_labels = choices;
      f[0].choice_count = 1;
      f[3].default_value.integer = 1;
      f[3].max_bytes = 0;
      return f;
    }();
    *out = {{sizeof(*out), 3},
            Bytes("mages"),
            Bytes("impacto"),
            Bytes("0.1.0"),
            ShinkiEmuCAP_CPU_SURFACE,
            formats,
            1,
            fields.data(),
            fields.size(),
            Bytes("Impacto"),
            Bytes("MAGES."),
            {}};
    return ShinkiEmuOK;
  });
}
ShinkiEmuStatus Probe(ShinkiEmuBytes game, ShinkiEmuProbeResult* out,
                     ShinkiEmuError* e) noexcept {
  return Guard(e, [&]() -> ShinkiEmuStatus {
    if (!out) throw std::invalid_argument("Null probe result");
    *out = {};
    fs::path p = Absolute(Text(game));
    if (fs::is_regular_file(p)) p = p.parent_path();
    if (fs::is_directory(p / "USRDIR")) p /= "USRDIR";
    for (auto name : {"script.mpk", "system.mpk", "bg1.mpk", "voice.mpk"})
      if (!IsMpk(p / name)) return ShinkiEmuNO_MATCH;
    *out = {Bytes("chaos-child"), Bytes("mpk"), 9900, 0};
    return ShinkiEmuOK;
  });
}
fs::path DefaultResourceRoot() {
#ifdef _WIN32
  static int ModuleAnchor;
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&ModuleAnchor), &module) ||
      module == GetModuleHandleW(nullptr))
    throw std::invalid_argument(
        "resources_root is required when no dynamic core module can be "
        "located");
  std::array<wchar_t, 32768> path{};
  DWORD length =
      GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (!length || length >= path.size())
    throw std::invalid_argument(
        "resources_root is required because the dynamic core path is "
        "unavailable");
  return fs::weakly_canonical(
      fs::path(std::wstring_view(path.data(), length)).parent_path() /
      "Resources");
#else
  throw std::invalid_argument("resources_root is required on this platform");
#endif
}
ShinkiEmuStatus Open(ShinkiEmuOpenRequest const* r, ShinkiEmuOpenResult* out,
                    ShinkiEmuError* e) noexcept {
  if (out) *out = {};
  return Guard(e, [&]() -> ShinkiEmuStatus {
    if (!r || !out || r->header.size != sizeof(*r) || r->header.version != 3 ||
        !r->session_epoch || r->host.header.size != sizeof(r->host) ||
        r->host.header.version != 3 || !r->host.cancellation.is_cancelled ||
        r->configuration_count > ShinkiEmuMAX_CONFIG_FIELDS ||
        (r->configuration_count && !r->configuration) ||
        r->render_extent.width || r->render_extent.height || r->module_owner)
      throw std::invalid_argument("Invalid ABI v3 open request");
    auto s = std::make_unique<Session>();
    s->epoch = r->session_epoch;
    s->host = r->host;
    if (r->window.focused > 1 || r->window.suspended > 1)
      throw std::invalid_argument("Invalid initial host window state");
    s->suspended = r->window.suspended != 0;
    fs::path game = Absolute(Text(r->game_path));
    if (fs::is_regular_file(game)) game = game.parent_path();
    s->settings.assets =
        fs::is_directory(game / "USRDIR") ? game / "USRDIR" : game;
    std::set<std::string> seen;
    for (size_t i = 0; i < r->configuration_count; ++i) {
      auto const& entry = r->configuration[i];
      auto id = Text(entry.id);
      if (!seen.insert(id).second || entry.value.reserved)
        throw std::invalid_argument("Duplicate or reserved config entry");
      auto const& v = entry.value;
      if (id == "use_backup_archives") {
        if (v.kind != ShinkiEmuVALUE_BOOL || (v.integer != 0 && v.integer != 1))
          throw std::invalid_argument("Expected boolean use_backup_archives");
        s->settings.backups = v.integer != 0;
      } else if (id == "profile") {
        if (v.kind != ShinkiEmuVALUE_ENUM || Text(v.text) != "cc")
          throw std::invalid_argument("Only native profile=cc is supported");
      } else {
        if (v.kind != ShinkiEmuVALUE_STRING)
          throw std::invalid_argument("Expected string path configuration");
        auto text = Text(v.text);
        if (id == "assets_root") {
          if (!text.empty()) s->settings.assets = Absolute(text);
        } else if (id == "resources_root") {
          if (!text.empty()) s->settings.resources = Absolute(text);
        } else if (id == "movie_order") {
          if (!text.empty()) s->settings.movie_order = Absolute(text);
        } else
          throw std::invalid_argument("Unknown configuration field: " + id);
      }
    }
    if (s->settings.resources.empty())
      s->settings.resources = DefaultResourceRoot();
    ValidateAssets(s->settings);
    for (auto& frame : s->frames) frame = FrameRef(new Frame());
    if (s->host.cancellation.is_cancelled(s->host.cancellation.context,
                                          s->epoch))
      return Error(e, ShinkiEmuCANCELLED, "impacto.cancelled", "Open cancelled");
    {
      std::lock_guard lock(GlobalMutex);
      if (Active)
        return Error(e, ShinkiEmuINVALID_STATE, "impacto.active",
                     "Only one native Impacto session can run");
      Active = s.get();
    }
    s->worker = std::thread(Run, s.get());
    bool stopped;
    {
      std::unique_lock lock(s->mutex);
      s->ready.wait(lock, [&] { return s->initialized || s->done; });
      stopped = s->done;
    }
    if (stopped || s->host.cancellation.is_cancelled(
                       s->host.cancellation.context, s->epoch)) {
      s->cancelled = true;
      s->worker.join();
      if (s->status == ShinkiEmuSESSION_FAILED) {
        if (e) *e = s->failure;
        return ShinkiEmuCORE_ERROR;
      }
      return Error(e, ShinkiEmuCANCELLED, "impacto.cancelled", "Open cancelled");
    }
    *out = {s.get(), 1, 1, Surface()};
    s.release();
    return ShinkiEmuOK;
  });
}
SDL_Scancode Scan(std::string const& key) {
  if (key.size() == 4 && key.starts_with("Key") && key[3] >= 'A' &&
      key[3] <= 'Z')
    return static_cast<SDL_Scancode>(SDL_SCANCODE_A + key[3] - 'A');
  if (key.size() == 6 && key.starts_with("Digit")) {
    if (key[5] == '0') return SDL_SCANCODE_0;
    if (key[5] >= '1' && key[5] <= '9')
      return static_cast<SDL_Scancode>(SDL_SCANCODE_1 + key[5] - '1');
  }
  static constexpr std::pair<std::string_view, SDL_Scancode> names[] = {
      {"Enter", SDL_SCANCODE_RETURN},
      {"Escape", SDL_SCANCODE_ESCAPE},
      {"Space", SDL_SCANCODE_SPACE},
      {"ArrowUp", SDL_SCANCODE_UP},
      {"ArrowDown", SDL_SCANCODE_DOWN},
      {"ArrowLeft", SDL_SCANCODE_LEFT},
      {"ArrowRight", SDL_SCANCODE_RIGHT},
      {"ControlLeft", SDL_SCANCODE_LCTRL},
      {"ControlRight", SDL_SCANCODE_RCTRL},
      {"ShiftLeft", SDL_SCANCODE_LSHIFT},
      {"ShiftRight", SDL_SCANCODE_RSHIFT},
      {"Tab", SDL_SCANCODE_TAB},
      {"Backspace", SDL_SCANCODE_BACKSPACE},
      {"PageUp", SDL_SCANCODE_PAGEUP},
      {"PageDown", SDL_SCANCODE_PAGEDOWN},
      {"Home", SDL_SCANCODE_HOME},
      {"End", SDL_SCANCODE_END},
      {"Delete", SDL_SCANCODE_DELETE},
      {"Insert", SDL_SCANCODE_INSERT},
      {"AltLeft", SDL_SCANCODE_LALT},
      {"AltRight", SDL_SCANCODE_RALT},
      {"Minus", SDL_SCANCODE_MINUS},
      {"Equal", SDL_SCANCODE_EQUALS},
      {"Backquote", SDL_SCANCODE_GRAVE},
      {"Comma", SDL_SCANCODE_COMMA},
      {"Period", SDL_SCANCODE_PERIOD},
      {"Slash", SDL_SCANCODE_SLASH},
      {"Semicolon", SDL_SCANCODE_SEMICOLON},
      {"Quote", SDL_SCANCODE_APOSTROPHE},
      {"Backslash", SDL_SCANCODE_BACKSLASH},
      {"BracketLeft", SDL_SCANCODE_LEFTBRACKET},
      {"BracketRight", SDL_SCANCODE_RIGHTBRACKET}};
  for (auto [name, code] : names)
    if (key == name) return code;
  if (key.size() >= 2 && key[0] == 'F') {
    auto n = key.substr(1);
    for (int i = 1; i <= 12; ++i)
      if (n == std::to_string(i))
        return static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + i - 1);
  }
  return SDL_SCANCODE_UNKNOWN;
}
ShinkiEmuStatus InputEvents(ShinkiEmuSession handle, ShinkiEmuInputEvent const* in,
                           size_t count, ShinkiEmuError* e) noexcept {
  return Guard(e, [&]() -> ShinkiEmuStatus {
    if (!handle || (count && !in) || count > 4096)
      throw std::invalid_argument("Invalid input batch");
    auto* s = static_cast<Session*>(handle);
    auto& converted = s->input_scratch;
    converted.clear();
    converted.reserve(count);
    std::optional<bool> suspension;
    for (size_t i = 0; i < count; ++i) {
      auto const& input = in[i];
      SDL_Event event{};
      if (input.pressed > 1 || input.repeat > 1 || !std::isfinite(input.x) ||
          !std::isfinite(input.y))
        throw std::invalid_argument("Invalid input value");
      if (input.kind != ShinkiEmuINPUT_KEY &&
          input.kind != ShinkiEmuINPUT_WINDOW &&
          (input.displayed.session_epoch != s->epoch ||
           input.displayed.output_epoch != 1))
        return Error(e, ShinkiEmuSTALE_CONTEXT, "impacto.frame_context",
                     "Input refers to another session or output");
      switch (input.kind) {
        case ShinkiEmuINPUT_KEY:
          event.type = input.pressed ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
          event.key.scancode = Scan(Text(input.physical_key));
          if (event.key.scancode == SDL_SCANCODE_UNKNOWN)
            return Error(e, ShinkiEmuUNSUPPORTED, "impacto.key",
                         "Unsupported physical key");
          event.key.down = input.pressed != 0;
          event.key.repeat = input.repeat != 0;
          break;
        case ShinkiEmuINPUT_POINTER_MOVE:
          event.type = SDL_EVENT_MOUSE_MOTION;
          event.motion.x = static_cast<float>(input.x);
          event.motion.y = static_cast<float>(input.y);
          break;
        case ShinkiEmuINPUT_POINTER_BUTTON:
          if (input.button < 1 || input.button > 3)
            throw std::invalid_argument("Invalid pointer button");
          event.type = input.pressed ? SDL_EVENT_MOUSE_BUTTON_DOWN
                                     : SDL_EVENT_MOUSE_BUTTON_UP;
          event.button.button =
              input.button == ShinkiEmuBUTTON_PRIMARY     ? SDL_BUTTON_LEFT
              : input.button == ShinkiEmuBUTTON_SECONDARY ? SDL_BUTTON_RIGHT
                                                         : SDL_BUTTON_MIDDLE;
          event.button.down = input.pressed != 0;
          event.button.x = static_cast<float>(input.x);
          event.button.y = static_cast<float>(input.y);
          break;
        case ShinkiEmuINPUT_WHEEL:
          event.type = SDL_EVENT_MOUSE_WHEEL;
          event.wheel.x = static_cast<float>(input.x);
          event.wheel.y = static_cast<float>(input.y);
          break;
        case ShinkiEmuINPUT_WINDOW:
          if (input.window.focused > 1 || input.window.suspended > 1 ||
              !std::isfinite(input.window.scale_factor) ||
              input.window.scale_factor <= 0)
            throw std::invalid_argument("Invalid host window state");
          suspension = input.window.suspended != 0;
          event.type = input.window.focused ? SDL_EVENT_WINDOW_FOCUS_GAINED
                                            : SDL_EVENT_WINDOW_FOCUS_LOST;
          break;
        default:
          throw std::invalid_argument("Unknown input kind");
      }
      converted.push_back(event);
    }
    std::lock_guard lock(s->mutex);
    if (s->done)
      return Error(e, ShinkiEmuINVALID_STATE, "impacto.finished",
                   "Native session has stopped");
    if (s->events.size() + converted.size() > 8192)
      return Error(e, ShinkiEmuCAPACITY, "impacto.input_capacity",
                   "Native input queue is full");
    s->events.insert(s->events.end(), converted.begin(), converted.end());
    if (suspension) s->suspended = *suspension;
    return ShinkiEmuOK;
  });
}
ShinkiEmuStatus Control(ShinkiEmuSession handle, uint32_t command,
                       ShinkiEmuError* e) noexcept {
  return Guard(e, [&]() -> ShinkiEmuStatus {
    if (!handle) throw std::invalid_argument("Null session");
    auto* s = static_cast<Session*>(handle);
    std::lock_guard lock(s->mutex);
    if (s->done)
      return Error(e, ShinkiEmuINVALID_STATE, "impacto.finished",
                   "Native session has stopped");
    if (command == ShinkiEmuCONTROL_PAUSE)
      s->paused = true;
    else if (command == ShinkiEmuCONTROL_RESUME)
      s->paused = false;
    else
      return Error(e, ShinkiEmuUNSUPPORTED, "impacto.control",
                   "Unsupported native control");
    return ShinkiEmuOK;
  });
}
ShinkiEmuStatus State(ShinkiEmuSession handle, ShinkiEmuSessionState* out,
                     ShinkiEmuError* e) noexcept {
  return Guard(e, [&]() -> ShinkiEmuStatus {
    if (!handle || !out) throw std::invalid_argument("Null session state");
    auto* s = static_cast<Session*>(handle);
    std::lock_guard lock(s->mutex);
    *out = {s->status, 0, 1, 1, s->failure};
    return ShinkiEmuOK;
  });
}
void ReleaseFrame(void* owner) noexcept {
  DropFrame(static_cast<Frame*>(owner));
}
ShinkiEmuStatus GetFrame(ShinkiEmuSession handle, ShinkiEmuFrameIdentity previous,
                        ShinkiEmuFrameSurface* out, ShinkiEmuError* e) noexcept {
  return Guard(e, [&]() -> ShinkiEmuStatus {
    if (!handle || !out) throw std::invalid_argument("Null frame request");
    *out = {};
    auto* s = static_cast<Session*>(handle);
    std::lock_guard lock(s->mutex);
    if (!s->frame) {
      out->state = ShinkiEmuFRAME_PENDING;
      return ShinkiEmuOK;
    }
    auto const& f = s->frame;
    out->identity = {s->epoch, 1, f->generation};
    out->timestamp_ns = f->timestamp;
    out->info = Surface(f->width, f->height);
    if (previous.session_epoch == s->epoch && previous.output_epoch == 1 &&
        previous.generation == f->generation) {
      out->state = ShinkiEmuFRAME_UNCHANGED;
      return ShinkiEmuOK;
    }
    RetainFrame(f);
    out->state = ShinkiEmuFRAME_READY;
    out->cpu = {{f->pixels.data(), f->pixels.size()}, f, ReleaseFrame};
    return ShinkiEmuOK;
  });
}
void Cancel(ShinkiEmuSession handle) noexcept {
  if (handle) static_cast<Session*>(handle)->cancelled = true;
}
ShinkiEmuStatus Close(ShinkiEmuSession handle, ShinkiEmuError* e) noexcept {
  return Guard(e, [&]() -> ShinkiEmuStatus {
    if (!handle) throw std::invalid_argument("Null session");
    std::unique_ptr<Session> s(static_cast<Session*>(handle));
    s->cancelled = true;
    if (s->worker.joinable()) s->worker.join();
    if (s->status == ShinkiEmuSESSION_FAILED) {
      if (e) *e = s->failure;
      return ShinkiEmuCORE_ERROR;
    }
    return ShinkiEmuOK;
  });
}
ShinkiEmuStatus Extension(ShinkiEmuBytes, uint32_t, void const** out) noexcept {
  if (out) *out = nullptr;
  return ShinkiEmuUNSUPPORTED;
}
ShinkiEmuCoreApi const Api = {{sizeof(ShinkiEmuCoreApi), 3},
                             Initialize,
                             Describe,
                             Probe,
                             Open,
                             InputEvents,
                             Control,
                             State,
                             GetFrame,
                             Cancel,
                             Close,
                             Extension};
}  // namespace
namespace Impacto::Shinki {
void CheckCancellation() {
  if (Closing) return;
  if (NativeSession &&
      (NativeSession->cancelled ||
       NativeSession->host.cancellation.is_cancelled(
           NativeSession->host.cancellation.context, NativeSession->epoch)))
    throw Cancelled{};
}
bool IsClosing() { return Closing; }
[[noreturn]] void NativeExit(int code) {
  throw std::runtime_error("Native Impacto requested termination (" +
                           std::to_string(code) + ")");
}
std::string ResolveResource(std::string const& path) {
  if (!NativeSession) return path;
  CheckCancellation();
  fs::path p = fs::u8path(path);
  if (p.is_absolute()) return path;
  p = p.lexically_normal();
  auto first = p.begin();
  if (first == p.end()) return path;
  if (*first == "resources")
    return (NativeSession->settings.resources / p).generic_string();
  if (*first == "shaders")
    return (NativeSession->settings.resources / "src" / p).generic_string();
  return path;
}
void ConfigureProfile() {
  CheckCancellation();
  auto const& settings = NativeSession->settings;
  auto a = settings.assets.generic_string();
  auto archive = [&](std::string_view name) {
    return LuaString(a + "/" + std::string(name));
  };
  std::string script = settings.backups ? "script_bak.mpk" : "script.mpk",
              system = settings.backups ? "system_bak.mpk" : "system.mpk";
  Lua("root.Vfs.Mounts={script={" + archive(script) + "},system={" +
      archive(system) + "},bg={" + archive("bg1.mpk") + "," +
      archive("bg2.mpk") + "}," + "bgm={" + archive("bgm.mpk") + "},se={" +
      archive("se.mpk") + "},sysse={" + archive("sysse.mpk") + "},voice={" +
      archive("voice.mpk") + "}," + "chara={" + archive("chara.mpk") +
      "},mask={" + archive("mask.mpk") + "}}");
}
void CaptureFrame(int width, int height) {
  CheckCancellation();
  auto& s = *NativeSession;
  if (width != 1920 || height != 1080)
    throw std::runtime_error("Unexpected hidden CC framebuffer extent");
  Frame* target = nullptr;
  {
    std::lock_guard lock(s.mutex);
    for (auto const& owner : s.frames) {
      Frame* candidate = owner.get();
      if (candidate != s.frame &&
          candidate->references.load(std::memory_order_acquire) == 1) {
        target = candidate;
        break;
      }
    }
    if (!target) return;
  }
  GLC::BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  glReadBuffer(GL_BACK);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
               target->pixels.data());
  if (glGetError() != GL_NO_ERROR)
    throw std::runtime_error("Native GL framebuffer readback failed");
  target->width = width;
  target->height = height;
  target->timestamp = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
  {
    std::lock_guard lock(s.mutex);
    target->generation = s.frame ? s.frame->generation + 1 : 1;
    s.frame = target;
  }
}
}  // namespace Impacto::Shinki
#ifdef _WIN32
#define SHINKI_EMU_EXPORT extern "C" __declspec(dllexport)
#else
#define SHINKI_EMU_EXPORT extern "C" __attribute__((visibility("default")))
#endif
SHINKI_EMU_EXPORT ShinkiEmuStatus
shinki_emu_impacto_query(uint32_t version, ShinkiEmuCoreApi const** out) noexcept {
  if (!out) return ShinkiEmuINVALID_ARGUMENT;
  *out = nullptr;
  if (version != ShinkiEmuABI_VERSION) return ShinkiEmuUNSUPPORTED;
  *out = &Api;
  return ShinkiEmuOK;
}
SHINKI_EMU_EXPORT ShinkiEmuStatus
shinki_emu_query(uint32_t version, ShinkiEmuCoreApi const** out) noexcept {
  return shinki_emu_impacto_query(version, out);
}
