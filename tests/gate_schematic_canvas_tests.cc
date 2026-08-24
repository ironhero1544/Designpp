// Copyright 2026 The Design++ Authors

#include <CppUnitTest.h>
#include <windows.h>

#include <chrono>
#include <memory>
#include <string>

#include "designpp/gui/gate_schematic_canvas.h"
#include "designpp/gui/schematic_scene.h"

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace designpp::tests {
namespace {

std::shared_ptr<const gui::SchematicScene> MakeStressScene() {
  auto scene = std::make_shared<gui::SchematicScene>();
  scene->top_module = "generated_stress";
  scene->mode = gui::SchematicViewMode::kReadable;
  scene->width = 12'200;
  scene->height = 5'200;
  scene->nodes.reserve(1'800);
  for (int index = 0; index < 1'800; ++index) {
    const int column = index % 60;
    const int row = index / 60;
    gui::SchematicSceneNode node;
    node.id = "gate_" + std::to_string(index);
    node.type = "$_AND_";
    node.label = "AND";
    node.instance_label = "U" + std::to_string(index + 1);
    node.kind = core::SchematicNodeKind::kPrimitive;
    node.bounds = {80 + column * 200, 80 + row * 168, 80 + column * 200 + 112,
                   80 + row * 168 + 96};
    scene->nodes.push_back(std::move(node));
  }
  scene->routing.routes.reserve(7'200);
  for (int index = 0; index < 7'200; ++index) {
    const int row = index % 900;
    const int y = 40 + row * 5;
    scene->routing.routes.push_back(
        {"net_" + std::to_string(index), {{20, y}, {12'000, y}}});
  }
  return scene;
}

class HiddenCanvasHost final {
 public:
  HiddenCanvasHost() {
    window_ = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, -32'000, -32'000,
                              1'280, 720, nullptr, nullptr,
                              GetModuleHandleW(nullptr), nullptr);
    if (window_ != nullptr) {
      static_cast<void>(canvas_.Create(GetModuleHandleW(nullptr), window_));
      canvas_.Move(0, 0, 1'280, 720);
      ShowWindow(window_, SW_SHOWNOACTIVATE);
    }
  }

  ~HiddenCanvasHost() {
    if (window_ != nullptr) DestroyWindow(window_);
  }

  [[nodiscard]] bool valid() const {
    return window_ != nullptr && canvas_.Window() != nullptr;
  }
  [[nodiscard]] gui::GateSchematicCanvas* canvas() { return &canvas_; }

  void PaintNow() {
    RedrawWindow(canvas_.Window(), nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
  }

 private:
  HWND window_ = nullptr;
  gui::GateSchematicCanvas canvas_;
};

}  // namespace

TEST_CLASS(GateSchematicCanvasTests){
  public : TEST_METHOD(RepeatedFitZoomAndPaintDoesNotLeakGdiObjects){
      HiddenCanvasHost host;
Assert::IsTrue(host.valid());
host.canvas()->SetScene(MakeStressScene());
host.PaintNow();
const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
for (int iteration = 0; iteration < 100; ++iteration) {
  SendMessageW(host.canvas()->Window(), WM_KEYDOWN, L'F', 0);
  const short wheel = iteration % 2 == 0 ? WHEEL_DELTA : -WHEEL_DELTA;
  SendMessageW(host.canvas()->Window(), WM_MOUSEWHEEL, MAKEWPARAM(0, wheel),
               MAKELPARAM(640, 360));
  host.PaintNow();
}
const DWORD after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
Assert::IsTrue(after <= before + 2);
}  // namespace designpp::tests

BEGIN_TEST_METHOD_ATTRIBUTE(StressScenePaintsWithinReleaseBudget)
TEST_METHOD_ATTRIBUTE(L"Category", L"Performance")
END_TEST_METHOD_ATTRIBUTE()

TEST_METHOD(StressScenePaintsWithinReleaseBudget) {
  HiddenCanvasHost host;
  Assert::IsTrue(host.valid());
  host.canvas()->SetScene(MakeStressScene());
  host.PaintNow();
  const auto started = std::chrono::steady_clock::now();
  for (int iteration = 0; iteration < 20; ++iteration) host.PaintNow();
  const auto elapsed = std::chrono::steady_clock::now() - started;
#ifdef NDEBUG
  Assert::IsTrue(elapsed < std::chrono::seconds(5));
#else
  Assert::IsTrue(elapsed < std::chrono::seconds(15));
#endif
}
}
;

}  // namespace designpp::tests
