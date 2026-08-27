#include "graphics/guest_gpu/hardwareContext.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace {

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "ShaderStagesEnTests: failed: %s\n", text);
    std::abort();
  }
}

using Libs::Graphics::HW::DecodeShaderStages;
using Libs::Graphics::HW::ShaderStagesEn;

// Every stage word below was observed in a census over five titles, so these are values the
// hardware really produces rather than constructed examples.

void OrdinaryNggVertexDrawIsVertexOnly() {
  // The majority of PS5 draws: PRIMGEN_EN plus passthrough, no stage enables.
  const auto s = DecodeShaderStages(0x02002000u);

  Check(s.primgen_en, "0x02002000 sets PRIMGEN_EN");
  Check(s.ngg_passthrough, "0x02002000 sets the NGG passthrough bit");
  Check(s.stage_enables == ShaderStagesEn::STAGE_ENABLES_NONE,
        "0x02002000 enables no stage");
  Check(s.IsNggVertexOnly(), "0x02002000 is a vertex-only NGG draw");
  Check(!s.IsNggMergedEsGs(), "0x02002000 is not a merged ES+GS draw");
  Check(s.VertexWaveSize() == 64u, "0x02002000 is a wave64 vertex draw");
}

void WaveSizeBitsDoNotChangePipelineShape() {
  // The regression the decode exists to prevent: an exact match against 0x02002000 rejected
  // these outright, costing Dead Space 14.8% of its early draws.
  const auto plain = DecodeShaderStages(0x02002000u);
  const auto wave32 = DecodeShaderStages(0x02402000u);

  Check(wave32.IsNggVertexOnly(), "0x02402000 is still a vertex-only NGG draw");
  Check(wave32.gs_w32_en, "0x02402000 sets GS_W32_EN");
  Check(wave32.VertexWaveSize() == 32u, "0x02402000 is a wave32 vertex draw");
  Check(plain.stage_enables == wave32.stage_enables,
        "the wave-size bit leaves the stage enables untouched");
  Check(plain.IsNggVertexOnly() == wave32.IsNggVertexOnly(),
        "the wave-size bit does not change the classification");

  // The other two width bits must be equally inert.
  for (const uint32_t bit : {ShaderStagesEn::HS_W32_EN, ShaderStagesEn::VS_W32_EN}) {
    const auto s = DecodeShaderStages(0x02002000u | bit);
    Check(s.IsNggVertexOnly(), "a width bit alone does not disqualify a vertex draw");
    Check(s.stage_enables == ShaderStagesEn::STAGE_ENABLES_NONE,
          "a width bit does not leak into the stage enables");
  }
}

void MergedEsGsIsRecognised() {
  // Stray and Silent Hill both emit this word. It does not set the passthrough bit, which is
  // what separates a real geometry program from a vertex-only wave.
  const auto s = DecodeShaderStages(0x00002030u);

  Check(s.primgen_en, "0x00002030 sets PRIMGEN_EN");
  Check(!s.ngg_passthrough, "0x00002030 does not set the NGG passthrough bit");
  Check(s.stage_enables == ShaderStagesEn::STAGE_ENABLES_ES_GS,
        "0x00002030 enables ES and GS");
  Check(s.IsNggMergedEsGs(), "0x00002030 is a merged ES+GS draw");
  Check(!s.IsNggVertexOnly(), "0x00002030 is not a vertex-only draw");
}

void TheTwoConfigurationsAreDisjoint() {
  // A word can never satisfy both predicates, whatever the width bits say.
  for (const uint32_t raw : {0x02002000u, 0x02402000u, 0x00002030u, 0x00000000u,
                             0x02002030u, 0x00002000u}) {
    const auto s = DecodeShaderStages(raw);
    Check(!(s.IsNggVertexOnly() && s.IsNggMergedEsGs()),
          "no stage word is both vertex-only and merged ES+GS");
  }
}

void UnrecognisedWordsAreNeitherConfiguration() {
  // Neither a zero word nor a contradictory one may be mistaken for a supported config.
  const auto zero = DecodeShaderStages(0x00000000u);
  Check(!zero.IsNggVertexOnly(), "a zero stage word is not a vertex-only draw");
  Check(!zero.IsNggMergedEsGs(), "a zero stage word is not a merged ES+GS draw");

  const auto contradictory = DecodeShaderStages(0x02002030u);
  Check(!contradictory.IsNggVertexOnly(),
        "passthrough with stage enables is not a vertex-only draw");
  Check(!contradictory.IsNggMergedEsGs(),
        "passthrough with stage enables is not a merged ES+GS draw");

  // PRIMGEN alone, without passthrough and without stage enables.
  const auto primgen_only = DecodeShaderStages(0x00002000u);
  Check(!primgen_only.IsNggVertexOnly(),
        "PRIMGEN without passthrough is not a vertex-only draw");
  Check(!primgen_only.IsNggMergedEsGs(),
        "PRIMGEN without stage enables is not a merged ES+GS draw");
}

void RawValueIsPreserved() {
  const auto s = DecodeShaderStages(0x02402000u);
  Check(s.raw == 0x02402000u, "the raw register value is carried through the decode");
}

} // namespace

int main() {
  OrdinaryNggVertexDrawIsVertexOnly();
  WaveSizeBitsDoNotChangePipelineShape();
  MergedEsGsIsRecognised();
  TheTwoConfigurationsAreDisjoint();
  UnrecognisedWordsAreNeitherConfiguration();
  RawValueIsPreserved();

  std::printf("ShaderStagesEnTests: ok\n");
  return 0;
}
