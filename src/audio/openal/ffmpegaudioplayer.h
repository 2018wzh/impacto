#pragma once

#include "audiocommon.h"

#include "../ffmpegaudioplayer.h"

namespace Impacto {
namespace Audio {
namespace OpenAL {

class FFmpegAudioPlayer : public Audio::FFmpegAudioPlayer {
 public:
  FFmpegAudioPlayer(Video::FFmpegPlayer* player)
      : Audio::FFmpegAudioPlayer(player) {}
  ~FFmpegAudioPlayer();

  void Init() override;
  void InitConvertContext(AVCodecContext* codecCtx) override;
  void FillAudioBuffers() override;
  void Process() override;
  void Reinit() override;

  void Stop() override;
  void Unload() override;
  void SetPaused(bool paused) override;

 private:
  ALCuint ALSource = 0;
  static int constexpr AudioBufferSize = 256 * 8192;
  static int constexpr AudioBufferCount = 3;
  ALuint BufferIds[AudioBufferCount] = {};
  uint8_t HostBuffer[AudioBufferSize];
  int FirstFreeBuffer = 0;
  int FreeBufferCount = AudioBufferCount;
  int BufferStartPositions[AudioBufferCount] = {};
  bool First = true;
  bool ResumeAfterPause = false;
};

}  // namespace OpenAL
}  // namespace Audio
}  // namespace Impacto
