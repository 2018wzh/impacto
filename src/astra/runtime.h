#pragma once
#include <string>
#include <cstdint>
namespace Impacto::Astra {
// Called only on the native worker, before the hidden backbuffer is swapped.
void CaptureFrame(int width, int height);
std::string ResolveResource(std::string const& path);
void ConfigureProfile();
void CheckCancellation();
bool IsClosing();
[[noreturn]] void NativeExit(int code);
}  // namespace Impacto::Astra
