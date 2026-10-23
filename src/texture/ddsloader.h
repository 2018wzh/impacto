#pragma once

#include "../io/io.h"
#include "texture.h"

namespace Impacto {
namespace Gxm {
// Decode two native DDS strips into one buffer without intermediate pixel copies.
bool TextureLoadDDSPair(Io::Stream* top, Io::Stream* bottom, Texture* outTexture);
}  // namespace Gxm
}  // namespace Impacto
