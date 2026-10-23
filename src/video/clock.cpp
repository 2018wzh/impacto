#include "clock.h"
#include "../impacto.h"

namespace Impacto::Video {

using namespace std::chrono;
Clock::Clock()
    : Pts(), LastUpdated(Now()), Speed(1.0), Serial(-1), Paused(false) {}

void Clock::SyncTo(Clock* target) {
  Microseconds clock = Get();
  Microseconds targetClock = target->Get();
  if (targetClock != Microseconds{} &&
      (clock == Microseconds{} || abs(clock - targetClock) > 10s))
    Set(targetClock, target->Serial);
}

void Clock::Set(Microseconds pts, int serial) {
  Pts = pts;
  LastUpdated = std::chrono::steady_clock::now();
  Serial = serial;
}

void Clock::SetPaused(bool paused) {
  if (Paused == paused) return;
  Set(Get(), Serial);
  Paused = paused;
}

Clock::Microseconds Clock::Get() {
  if (Paused) {
    return Pts;
  } else {
    auto elapsed = Now() - LastUpdated;
    auto scaled = duration<double, std::micro>(elapsed * Speed);
    return Pts + round<Microseconds>(scaled);
  }
}

}  // namespace Impacto::Video