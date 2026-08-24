// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>

#include <array>
#include <cmath>

#include "designpp/gui/schematic_viewport.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {

TEST_CLASS(SchematicViewportTests){
  public : TEST_METHOD(FitContainsLargeSceneAtCommonDpiScales){
      constexpr std::array<double, 4> kDpiScales{1.0, 1.25, 1.5, 2.0};
for (const double dpi_scale : kDpiScales) {
  const double zoom = gui::CalculateSchematicFitZoom(240'000, 90'000, 1'920,
                                                     1'080, dpi_scale, 28, 48);
  Assert::IsTrue(zoom >= 0.001);
  Assert::IsTrue(240'000 * zoom * dpi_scale <= 1'920 - 28 + 0.001);
  Assert::IsTrue(90'000 * zoom * dpi_scale <= 1'080 - 48 + 0.001);
}
}  // namespace designpp::tests

TEST_METHOD(FitSupportsExtremelyWideAndTallScenes) {
  const double wide =
      gui::CalculateSchematicFitZoom(1'000'000, 1'000, 1'280, 720, 1.0, 28, 48);
  const double tall =
      gui::CalculateSchematicFitZoom(1'000, 600'000, 1'280, 720, 1.0, 28, 48);
  Assert::IsTrue(wide <= 0.001252);
  Assert::IsTrue(tall <= 0.00112);
}

TEST_METHOD(ViewportSelectsDetailAndCullsGeometry) {
  const gui::SchematicViewport overview =
      gui::CalculateSchematicViewport(800, 600, 400, 200, 0.2, 48);
  Assert::IsTrue(overview.detail == gui::SchematicDetail::kOverview);
  Assert::IsTrue(gui::SchematicRectIsVisible({2'100, 1'100, 2'200, 1'200},
                                             overview.logical_bounds));
  Assert::IsFalse(gui::SchematicRectIsVisible({10'000, 10'000, 10'100, 10'100},
                                              overview.logical_bounds));
  Assert::IsTrue(gui::SchematicSegmentIsVisible({1'900, 1'000}, {6'100, 1'000},
                                                overview.logical_bounds));
  Assert::IsFalse(
      gui::SchematicPointIsVisible({20'000, 20'000}, overview.logical_bounds));

  Assert::IsTrue(
      gui::CalculateSchematicViewport(800, 600, 0, 0, 0.6, 0).detail ==
      gui::SchematicDetail::kSymbols);
  Assert::IsTrue(
      gui::CalculateSchematicViewport(800, 600, 0, 0, 1.0, 0).detail ==
      gui::SchematicDetail::kFull);
}

TEST_METHOD(InvalidDimensionsReturnSafeDefaults) {
  Assert::IsTrue(
      std::abs(gui::CalculateSchematicFitZoom(0, 500, 800, 600, 1.0, 28, 48) -
               1.0) < 0.0001);
  const gui::SchematicViewport viewport =
      gui::CalculateSchematicViewport(800, 600, 0, 0, 0.0, 48);
  Assert::AreEqual(0, viewport.logical_bounds.left);
  Assert::AreEqual(0, viewport.logical_bounds.top);
  Assert::AreEqual(0, viewport.logical_bounds.right);
  Assert::AreEqual(0, viewport.logical_bounds.bottom);
}
}
;

}  // namespace designpp::tests
