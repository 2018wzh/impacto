#include "mosaic.h"

#include "../profile/game.h"
#include "../renderer/renderer.h"

namespace Impacto {
namespace Effects {

MosaicEffect::MosaicEffect()
    : CaptureSprite(
          SpriteSheet(Profile::Game::DesignWidth, Profile::Game::DesignHeight),
          0.0f, 0.0f, Profile::Game::DesignWidth, Profile::Game::DesignHeight) {
  CaptureSprite.Sheet.IsScreenCap = true;
}

void MosaicEffect::Reset() {
  if (Renderer && CaptureSprite.Sheet.Texture) Renderer->FreeTexture(CaptureSprite.Sheet.Texture);
  CaptureSprite.Sheet.Texture = 0;
}

MosaicEffect::~MosaicEffect() { Reset(); }

void MosaicEffect::Init() {
  if (CaptureSprite.Sheet.Texture != 0) return;

  Texture texture{};
  texture.LoadSolidColor(static_cast<int>(Profile::Game::DesignWidth),
                         static_cast<int>(Profile::Game::DesignHeight), 0);
  CaptureSprite.Sheet.Texture = texture.Submit();
}

void MosaicEffect::Render(float tileSize) {
  if (tileSize <= 1.0f) return;

  tileSize = std::min(tileSize, 20.0f);

  assert(CaptureSprite.Sheet.Texture != 0);
  Renderer->CaptureScreencap(CaptureSprite);
  Renderer->DrawMosaic(CaptureSprite,
                       RectF{0.0f, 0.0f, Profile::Game::DesignWidth,
                             Profile::Game::DesignHeight},
                       tileSize, glm::mat4(1.0f), glm::vec4(1.0f));
}

}  // namespace Effects
}  // namespace Impacto
