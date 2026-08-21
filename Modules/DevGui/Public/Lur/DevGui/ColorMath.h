#pragma once
// Re-export of Lur::Render::ColorMath, which is where this lives now (#201) — colour belongs beside
// Render::Color, and RPS's team palette needed the same conversions without depending on the dev-GUI
// module to get them. Kept so the colour picker's call sites read unchanged.
#include "Lur/Render/ColorMath.h"

namespace Lur::DevGui::ColorMath {

using Lur::Render::ColorMath::Clamp01;
using Lur::Render::ColorMath::HsvToRgb;
using Lur::Render::ColorMath::RgbToHsv;
using Lur::Render::ColorMath::HueColor;

}  // namespace Lur::DevGui::ColorMath
