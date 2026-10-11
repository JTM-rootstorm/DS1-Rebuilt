#include "runtime/audio/mixer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "runtime/log.h"

namespace ds1::audio {

namespace {

constexpr int kChunk = 1024;  // frames decoded / mixed at a time

Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vec3 Normalize(const Vec3& v, const Vec3& fallback) {
  float l = std::sqrt(Dot(v, v));
  if (!(l > 1e-6f)) return fallback;
  return {v.x / l, v.y / l, v.z / l};
}

// Next frame of the current pass into dst[0..1] (mono is duplicated,
// positional stereo is folded to mono). False at the end of the pass.
bool Fetch(Voice& v, float* dst) {
  int ch = v.src->channels();
  if (v.buf_head_ * ch >= v.buf_.size()) {
    v.buf_.resize(size_t(kChunk) * ch);
    int got = v.src->Read(v.buf_.data(), kChunk);
    v.buf_.resize(size_t(std::max(got, 0)) * ch);
    v.buf_head_ = 0;
    if (got <= 0) return false;
  }
  const float* f = &v.buf_[v.buf_head_ * ch];
  v.buf_head_++;
  if (ch == 1) {
    dst[0] = dst[1] = f[0];
  } else if (v.positional) {
    dst[0] = dst[1] = 0.5f * (f[0] + f[1]);
  } else {
    dst[0] = f[0];
    dst[1] = f[1];
  }
  return true;
}

// Loads a_ and b_ at the current source position. False if the pass has
// no frames at all.
bool Prime(Voice& v) {
  v.primed_ = true;
  v.at_end_ = false;
  if (!Fetch(v, v.a_)) return false;
  if (!Fetch(v, v.b_)) {
    v.at_end_ = true;
    v.b_[0] = v.a_[0];
    v.b_[1] = v.a_[1];
  }
  return true;
}

void SDLCALL AudioCallback(void* ud, SDL_AudioStream* s, int additional, int) {
  auto* m = static_cast<Mixer*>(ud);
  float buf[kChunk * 2];
  int frames = additional / int(2 * sizeof(float));
  while (frames > 0) {
    int n = std::min(frames, kChunk);
    m->Mix(buf, n);
    SDL_PutAudioStreamData(s, buf, n * int(2 * sizeof(float)));
    frames -= n;
  }
}

}  // namespace

void Voice::Seek(int64_t f) {
  if (src) src->Seek(f);
  buf_.clear();
  buf_head_ = 0;
  pos_ = f;
  frac_ = 0;
  primed_ = false;
  at_end_ = false;
}

Mixer& GetMixer() {
  static Mixer m;
  return m;
}

bool Mixer::Open(int rate) {
  if (stream_) return true;
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    WarnF("audio: SDL audio init failed: %s\n", SDL_GetError());
    return false;
  }
  SDL_AudioSpec spec{SDL_AUDIO_F32, 2, rate};
  stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                      AudioCallback, this);
  if (!stream_) {
    WarnF("audio: cannot open a playback device: %s\n", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return false;
  }
  rate_ = rate;
  SDL_ResumeAudioStreamDevice(stream_);
  WarnF("audio: %d Hz stereo on SDL driver '%s'\n", rate,
        SDL_GetCurrentAudioDriver());
  return true;
}

void Mixer::Close() {
  if (!stream_) return;
  SDL_DestroyAudioStream(stream_);  // stops the callback first
  stream_ = nullptr;
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void Mixer::Add(Voice* v) {
  if (std::find(voices_.begin(), voices_.end(), v) == voices_.end())
    voices_.push_back(v);
}

void Mixer::Remove(Voice* v) {
  voices_.erase(std::remove(voices_.begin(), voices_.end(), v), voices_.end());
}

void Mixer::SetListener(const Vec3& pos, const Vec3& face, const Vec3& up) {
  lpos_ = pos;
  lface_ = Normalize(face, {0, 0, 1});
  lup_ = Normalize(up, {0, 1, 0});
}

void Mixer::TargetGains(const Voice& v, float* l, float* r) const {
  float g = v.volume * master_;
  if (!v.positional) {
    *l = *r = g;
    return;
  }
  // Inverse-distance rolloff from min_dist; inaudible past max_dist.
  Vec3 rel = Sub(v.pos, lpos_);
  float d = std::sqrt(Dot(rel, rel));
  float att = d <= v.min_dist ? 1.0f : d > v.max_dist ? 0.0f : v.min_dist / d;
  float p = 0;
  if (d > 1e-4f) {
    Vec3 right = Normalize(Cross(lup_, lface_), {1, 0, 0});
    p = std::clamp(Dot(rel, right) / d, -1.0f, 1.0f);
  }
  *l = g * att * std::min(1.0f, 1.0f - p);
  *r = g * att * std::min(1.0f, 1.0f + p);
}

void Mixer::MixVoice(Voice& v, float* out, int frames) {
  if (!v.src) {
    v.state = Voice::State::Done;
    v.eos = true;
    return;
  }
  if (!v.primed_ && !Prime(v)) {
    v.state = Voice::State::Done;
    v.eos = true;
    return;
  }
  float tl, tr;
  TargetGains(v, &tl, &tr);
  if (!v.gains_valid_) {
    v.cur_l_ = tl;
    v.cur_r_ = tr;
    v.gains_valid_ = true;
  }
  // Ramp gain changes across the chunk so volume and pan updates do not
  // click.
  float dl = (tl - v.cur_l_) / frames, dr = (tr - v.cur_r_) / frames;
  double rate = v.rate > 0 ? v.rate : v.src->rate();
  double step = rate / rate_;
  for (int i = 0; i < frames; i++) {
    float f = float(v.frac_);
    float sl = v.a_[0] + (v.b_[0] - v.a_[0]) * f;
    float sr = v.a_[1] + (v.b_[1] - v.a_[1]) * f;
    v.cur_l_ += dl;
    v.cur_r_ += dr;
    out[2 * i] += sl * v.cur_l_;
    out[2 * i + 1] += sr * v.cur_r_;
    v.frac_ += step;
    while (v.frac_ >= 1.0) {
      v.frac_ -= 1.0;
      if (v.at_end_) {
        // a_ was the last frame of this pass.
        v.passes++;
        if (v.loops == 0 || v.passes < v.loops) {
          v.Seek(0);
          if (Prime(v)) continue;
        }
        v.state = Voice::State::Done;
        v.eos = true;
        v.cur_l_ = tl;
        v.cur_r_ = tr;
        return;
      }
      v.a_[0] = v.b_[0];
      v.a_[1] = v.b_[1];
      v.pos_++;
      if (!Fetch(v, v.b_)) {
        v.at_end_ = true;
        v.b_[0] = v.a_[0];
        v.b_[1] = v.a_[1];
      }
    }
  }
  v.cur_l_ = tl;
  v.cur_r_ = tr;
}

void Mixer::Mix(float* out, int frames) {
  std::memset(out, 0, sizeof(float) * 2 * frames);
  std::lock_guard<std::mutex> l(mu_);
  for (Voice* v : voices_)
    if (v->state == Voice::State::Playing) MixVoice(*v, out, frames);
  for (int i = 0; i < 2 * frames; i++) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

}  // namespace ds1::audio
