#include "../spritesheet.h"

namespace Impacto {
namespace Effects {

class BlurEffect {
 public:
  void Render(int iterations);
  void Init();
  void Reset();

  BlurEffect();
  ~BlurEffect();

 private:
  Sprite BlurSprite;
};

inline BlurEffect Blur;

}  // namespace Effects
}  // namespace Impacto