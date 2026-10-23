#include "sprites.h"
#include "profile_internal.h"
#include "../io/assetpath.h"
#include "../log.h"
#include "../renderer/renderer.h"
#include "../texture/texture.h"
#include "../texture/ddsloader.h"
#include <future>
#include <memory>
#include <stdexcept>

namespace Impacto {
namespace Profile {

static Texture LoadTexture(Io::Stream* stream, std::string name) {
  Texture texture{};
  if (!texture.Load(stream)) {
    ImpLog(LogLevel::Error, LogChannel::Profile,
           "Spritesheet {:s} texture could not be imported, using fallback\n",
           name);
    texture.LoadCheckerboard();
  }

  delete stream;

  return texture;
}

static Texture LoadTexturePair(std::unique_ptr<Io::Stream> top,
                               std::unique_ptr<Io::Stream> bottom) {
  Texture texture{};
  if (!Gxm::TextureLoadDDSPair(top.get(), bottom.get(), &texture)) {
    throw std::runtime_error("DDS strips must have matching RGBA rows");
  }
  return texture;
}

void LoadSpritesheets() {
  EnsurePushMemberOfType("SpriteSheets", LUA_TTABLE);

  std::vector<std::tuple<std::string, std::future<Texture>>> futures;

  PushInitialIndex();
  while (PushNextTableElement() != 0) {
    std::string name(EnsureGetKey<std::string>());

    SpriteSheet& sheet = SpriteSheets[name];
    sheet.DesignWidth = EnsureGetMember<float>("DesignWidth");
    sheet.DesignHeight = EnsureGetMember<float>("DesignHeight");
    sheet.ScriptHandled = TryGetMember<bool>("ScriptHandled") == true;
    sheet.Path = EnsureGetMember<Io::AssetPath>("Path");

    if (sheet.ScriptHandled) {
      sheet.Texture = Renderer->MapSpriteSheet(sheet);
    } else {
      Io::Stream* stream;
      IoError err = sheet.Path.Open(&stream);
      if (err != IoError_OK) {
        ImpLog(LogLevel::Fatal, LogChannel::Profile,
               "Could not open spritesheet {:s}\n", name);
        Window->Shutdown();
      }

      if (auto appendPath = TryGetMember<Io::AssetPath>("AppendPath")) {
        std::unique_ptr<Io::Stream> top(stream);
        Io::Stream* rawBottom = nullptr;
        const IoError appendError = appendPath->Open(&rawBottom);
        std::unique_ptr<Io::Stream> bottom(rawBottom);
        if (appendError != IoError_OK) {
          throw std::runtime_error("Could not open appended native spritesheet");
        }
        futures.emplace_back(name, std::async(std::launch::async,
            LoadTexturePair, std::move(top), std::move(bottom)));
      } else {
        futures.emplace_back(std::tuple(
            name, std::async(std::launch::async, LoadTexture, stream, name)));
      }
    }
    Pop();
  }

  for (auto& [name, future] : futures) {
    SpriteSheet& sheet = SpriteSheets[name];
    sheet.Texture = future.get().Submit();
  }

  Pop();

  EnsurePushMemberOfType("Sprites", LUA_TTABLE);

  PushInitialIndex();
  while (PushNextTableElement() != 0) {
    std::string name(EnsureGetKey<std::string>());

    Sprite& sprite = Sprites[name];
    sprite.Sheet = EnsureGetMember<SpriteSheet>("Sheet");
    sprite.Bounds = EnsureGetMember<RectF>("Bounds");
    if (!TryGetMember<glm::vec2>("BaseScale", sprite.BaseScale))
      sprite.BaseScale = glm::vec2(1.0f);

    Pop();
  }

  Pop();
}

}  // namespace Profile
}  // namespace Impacto