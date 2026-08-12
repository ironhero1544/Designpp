// Copyright 2026 The Design++ Authors

#include "designpp/gui/gate_schematic_canvas.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "designpp/gui/orthogonal_edge_router.h"

namespace designpp::gui {
namespace {

constexpr wchar_t kCanvasClassName[] = L"DesignPlusPlus.GateSchematicCanvas";
constexpr int kLogicColumnSpacing = 192;
constexpr int kLogicRowSpacing = 112;
constexpr int kFirstLogicColumnX = 224;
constexpr COLORREF kCanvasBackground = RGB(255, 255, 255);
// Preblended against the white canvas at approximately 30% opacity.
constexpr COLORREF kGridMinorLine = RGB(236, 236, 236);
constexpr COLORREF kGridMajorLine = RGB(227, 227, 227);
constexpr COLORREF kSchematicBlack = RGB(24, 24, 24);
constexpr COLORREF kSecondaryText = RGB(96, 96, 96);
constexpr COLORREF kPinRed = RGB(196, 32, 38);
constexpr COLORREF kJunctionBlue = RGB(0, 102, 204);

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int length = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) return {};
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), length);
  return result;
}

int Scale(HWND window, int value) {
  return MulDiv(value, static_cast<int>(GetDpiForWindow(window)), 96);
}

void DrawSchematicGrid(HDC device, HWND window, RECT client, double zoom,
                       int scroll_x, int scroll_y) {
  const int base_spacing = Scale(window, 8);
  int step = 1;
  while (base_spacing * zoom * step < Scale(window, 8)) {
    step *= 2;
  }

  const double screen_spacing = base_spacing * zoom;
  const int first_column =
      static_cast<int>(std::floor(scroll_x / screen_spacing));
  const int last_column =
      static_cast<int>(std::ceil((scroll_x + client.right) / screen_spacing));
  const int first_row = static_cast<int>(std::floor(scroll_y / screen_spacing));
  const int last_row =
      static_cast<int>(std::ceil((scroll_y + client.bottom) / screen_spacing));

  HPEN minor_pen = CreatePen(PS_SOLID, 1, kGridMinorLine);
  HPEN major_pen = CreatePen(PS_SOLID, 1, kGridMajorLine);
  HGDIOBJ old_pen = SelectObject(device, minor_pen);
  for (int column = first_column; column <= last_column; ++column) {
    if (column % step != 0) continue;
    SelectObject(device, column % 5 == 0 ? major_pen : minor_pen);
    const int x =
        static_cast<int>(std::lround(column * screen_spacing)) - scroll_x;
    MoveToEx(device, x, client.top, nullptr);
    LineTo(device, x, client.bottom);
  }
  for (int row = first_row; row <= last_row; ++row) {
    if (row % step != 0) continue;
    SelectObject(device, row % 5 == 0 ? major_pen : minor_pen);
    const int y =
        static_cast<int>(std::lround(row * screen_spacing)) - scroll_y;
    MoveToEx(device, client.left, y, nullptr);
    LineTo(device, client.right, y);
  }
  SelectObject(device, old_pen);
  DeleteObject(major_pen);
  DeleteObject(minor_pen);
}

RECT GateBody(HWND window, RECT bounds) {
  bounds.left += Scale(window, 8);
  bounds.right -= Scale(window, 8);
  bounds.top += Scale(window, 16);
  bounds.bottom -= Scale(window, 18);
  return bounds;
}

int OrInputBoundaryX(HWND window, RECT body, LONG pin_y, bool xor_rear_curve) {
  const double height = std::max(1L, body.bottom - body.top);
  const double target = std::clamp((body.bottom - pin_y) / height, 0.0, 1.0);
  double lower = 0.0;
  double upper = 1.0;
  for (int iteration = 0; iteration < 16; ++iteration) {
    const double parameter = (lower + upper) * 0.5;
    const double position = parameter * parameter * parameter -
                            1.5 * parameter * parameter + 1.5 * parameter;
    if (position < target) {
      lower = parameter;
    } else {
      upper = parameter;
    }
  }
  const double parameter = (lower + upper) * 0.5;
  const double width = body.right - body.left;
  const int curve_x = body.left + static_cast<int>(0.75 * width * parameter *
                                                   (1.0 - parameter));
  return curve_x - (xor_rear_curve ? Scale(window, 5) : 0);
}

struct NodeLayout {
  RECT bounds{};
  const adapters::NetlistCell* cell = nullptr;
  std::wstring display_name;
  std::vector<std::pair<const adapters::NetlistCellPort*, POINT>> pins;
};

struct PortLayout {
  const adapters::NetlistPort* port = nullptr;
  std::string label;
  std::string bit;
  RECT bounds{};
};

enum class GateShape {
  kAnd,
  kOr,
  kXor,
  kNot,
  kBuffer,
  kGeneric,
};

struct GateAppearance {
  GateShape shape = GateShape::kGeneric;
  bool inverted_output = false;
  bool inverted_b_input = false;
  std::wstring label;
};

struct NetRoute {
  std::optional<POINT> driver;
  std::vector<POINT> sinks;
};

std::vector<int> CalculateLogicDepths(
    const adapters::GateSchematic& schematic) {
  std::map<std::string, int> net_depths;
  for (const adapters::NetlistPort& port : schematic.ports) {
    if (port.direction != adapters::NetlistPortDirection::kInput) continue;
    for (const std::string& bit : port.bits) net_depths[bit] = 0;
  }

  std::vector<int> depths(schematic.cells.size(), 0);
  std::size_t unresolved = schematic.cells.size();
  for (std::size_t pass = 0; pass < schematic.cells.size() && unresolved > 0;
       ++pass) {
    bool progressed = false;
    for (std::size_t index = 0; index < schematic.cells.size(); ++index) {
      if (depths[index] != 0) continue;
      const adapters::NetlistCell& cell = schematic.cells[index];
      int input_depth = 0;
      bool inputs_ready = true;
      for (const adapters::NetlistCellPort& port : cell.ports) {
        if (port.direction != adapters::NetlistPortDirection::kInput) continue;
        for (const std::string& bit : port.bits) {
          if (bit.starts_with("const:")) continue;
          const auto found = net_depths.find(bit);
          if (found == net_depths.end()) {
            inputs_ready = false;
            break;
          }
          input_depth = std::max(input_depth, found->second);
        }
        if (!inputs_ready) break;
      }
      if (!inputs_ready) continue;

      depths[index] = input_depth + 1;
      for (const adapters::NetlistCellPort& port : cell.ports) {
        if (port.direction == adapters::NetlistPortDirection::kInput) continue;
        for (const std::string& bit : port.bits) {
          net_depths.insert_or_assign(bit, depths[index]);
        }
      }
      --unresolved;
      progressed = true;
    }
    if (!progressed) break;
  }

  int fallback_depth = 1;
  for (const int depth : depths)
    fallback_depth = std::max(fallback_depth, depth);
  for (int& depth : depths) {
    if (depth == 0) depth = fallback_depth + 1;
  }
  return depths;
}

std::vector<int> CalculateVerticalRanks(
    const adapters::GateSchematic& schematic,
    const std::vector<int>& logic_depths) {
  std::map<std::string, double> input_positions;
  std::map<std::string, double> output_positions;
  int input_index = 0;
  int output_index = 0;
  for (const adapters::NetlistPort& port : schematic.ports) {
    const bool input = port.direction == adapters::NetlistPortDirection::kInput;
    for (const std::string& bit : port.bits) {
      if (input) {
        input_positions[bit] = static_cast<double>(input_index++);
      } else {
        output_positions[bit] = static_cast<double>(output_index++);
      }
    }
  }

  std::map<std::string, std::size_t> drivers;
  std::map<std::string, std::vector<std::size_t>> consumers;
  std::map<int, std::vector<std::size_t>> layers;
  for (std::size_t index = 0; index < schematic.cells.size(); ++index) {
    layers[logic_depths[index]].push_back(index);
    for (const adapters::NetlistCellPort& port : schematic.cells[index].ports) {
      for (const std::string& bit : port.bits) {
        if (bit.starts_with("const:")) continue;
        if (port.direction == adapters::NetlistPortDirection::kInput) {
          consumers[bit].push_back(index);
        } else {
          drivers.insert_or_assign(bit, index);
        }
      }
    }
  }

  std::vector<double> positions(schematic.cells.size(), 0.0);
  for (auto& [depth, layer] : layers) {
    static_cast<void>(depth);
    for (std::size_t row = 0; row < layer.size(); ++row) {
      positions[layer[row]] = static_cast<double>(row);
    }
  }

  const auto upstream_barycenter = [&](std::size_t cell_index) {
    double total = 0.0;
    int count = 0;
    for (const adapters::NetlistCellPort& port :
         schematic.cells[cell_index].ports) {
      if (port.direction != adapters::NetlistPortDirection::kInput) continue;
      for (const std::string& bit : port.bits) {
        const auto input = input_positions.find(bit);
        if (input != input_positions.end()) {
          total += input->second;
          ++count;
          continue;
        }
        const auto driver = drivers.find(bit);
        if (driver != drivers.end()) {
          total += positions[driver->second];
          ++count;
        }
      }
    }
    return count == 0 ? positions[cell_index] : total / count;
  };
  const auto downstream_barycenter = [&](std::size_t cell_index) {
    double total = 0.0;
    int count = 0;
    for (const adapters::NetlistCellPort& port :
         schematic.cells[cell_index].ports) {
      if (port.direction == adapters::NetlistPortDirection::kInput) continue;
      for (const std::string& bit : port.bits) {
        const auto output = output_positions.find(bit);
        if (output != output_positions.end()) {
          total += output->second;
          ++count;
        }
        const auto sinks = consumers.find(bit);
        if (sinks == consumers.end()) continue;
        for (const std::size_t sink : sinks->second) {
          total += positions[sink];
          ++count;
        }
      }
    }
    return count == 0 ? positions[cell_index] : total / count;
  };
  const auto sort_layer = [&positions](std::vector<std::size_t>* layer,
                                       const auto& barycenter) {
    std::stable_sort(layer->begin(), layer->end(),
                     [&barycenter](std::size_t left, std::size_t right) {
                       return barycenter(left) < barycenter(right);
                     });
    for (std::size_t row = 0; row < layer->size(); ++row) {
      positions[(*layer)[row]] = static_cast<double>(row);
    }
  };

  for (int iteration = 0; iteration < 4; ++iteration) {
    for (auto& [depth, layer] : layers) {
      static_cast<void>(depth);
      sort_layer(&layer, upstream_barycenter);
    }
    for (auto layer = layers.rbegin(); layer != layers.rend(); ++layer) {
      sort_layer(&layer->second, downstream_barycenter);
    }
  }

  std::vector<int> ranks(schematic.cells.size(), 0);
  for (const auto& [depth, layer] : layers) {
    static_cast<void>(depth);
    for (std::size_t row = 0; row < layer.size(); ++row) {
      ranks[layer[row]] = static_cast<int>(row);
    }
  }
  return ranks;
}

void DrawCenteredText(HDC device, const std::wstring& text, RECT bounds,
                      UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                                    DT_END_ELLIPSIS) {
  DrawTextW(device, text.c_str(), static_cast<int>(text.size()), &bounds,
            format | DT_NOPREFIX);
}

std::wstring InstanceDisplayName(std::size_t ordinal) {
  return L"U" + std::to_wstring(ordinal + 1U);
}

GateAppearance GetGateAppearance(std::string_view type) {
  GateAppearance appearance;
  appearance.label = Utf8ToWide(type);
  if (type.find("ANDNOT") != std::string_view::npos) {
    return {GateShape::kAnd, false, true, L"AND"};
  }
  if (type.find("ORNOT") != std::string_view::npos) {
    return {GateShape::kOr, false, true, L"OR"};
  }
  if (type.find("XNOR") != std::string_view::npos) {
    return {GateShape::kXor, true, false, L"XNOR"};
  }
  if (type.find("XOR") != std::string_view::npos) {
    return {GateShape::kXor, false, false, L"XOR"};
  }
  if (type.find("NAND") != std::string_view::npos) {
    return {GateShape::kAnd, true, false, L"NAND"};
  }
  if (type.find("AND") != std::string_view::npos) {
    return {GateShape::kAnd, false, false, L"AND"};
  }
  if (type.find("NOR") != std::string_view::npos) {
    return {GateShape::kOr, true, false, L"NOR"};
  }
  if (type.find("OR") != std::string_view::npos) {
    return {GateShape::kOr, false, false, L"OR"};
  }
  if (type.find("NOT") != std::string_view::npos) {
    return {GateShape::kNot, true, false, L"NOT"};
  }
  if (type.find("BUF") != std::string_view::npos) {
    return {GateShape::kBuffer, false, false, L"BUF"};
  }
  return appearance;
}

void DrawBezier(HDC device, POINT start, POINT control_one, POINT control_two,
                POINT end) {
  LineTo(device, start.x, start.y);
  POINT points[]{control_one, control_two, end};
  PolyBezierTo(device, points, 3);
}

void DrawGateSymbol(HDC device, HWND window, const NodeLayout& layout) {
  const GateAppearance appearance = GetGateAppearance(layout.cell->type);
  const RECT body = GateBody(window, layout.bounds);
  const int middle_y = (body.top + body.bottom) / 2;
  const int width = body.right - body.left;
  const int height = body.bottom - body.top;
  const int bubble_radius = Scale(window, 3);
  const int output_x =
      appearance.inverted_output ? body.right - bubble_radius * 2 : body.right;

  BeginPath(device);
  switch (appearance.shape) {
    case GateShape::kAnd:
      MoveToEx(device, body.left, body.top, nullptr);
      LineTo(device, body.left + width / 2, body.top);
      DrawBezier(device, {body.left + width / 2, body.top},
                 {body.left + (width * 78) / 100, body.top},
                 {output_x, body.top + (height * 22) / 100},
                 {output_x, middle_y});
      DrawBezier(device, {output_x, middle_y},
                 {output_x, body.bottom - (height * 22) / 100},
                 {body.left + (width * 78) / 100, body.bottom},
                 {body.left + width / 2, body.bottom});
      LineTo(device, body.left, body.bottom);
      LineTo(device, body.left, body.top);
      break;
    case GateShape::kOr:
    case GateShape::kXor:
      MoveToEx(device, body.left, body.top, nullptr);
      DrawBezier(
          device, {body.left, body.top}, {body.left + width / 2, body.top},
          {output_x - width / 5, body.top + height / 5}, {output_x, middle_y});
      DrawBezier(device, {output_x, middle_y},
                 {output_x - width / 5, body.bottom - height / 5},
                 {body.left + width / 2, body.bottom},
                 {body.left, body.bottom});
      DrawBezier(device, {body.left, body.bottom},
                 {body.left + width / 4, middle_y},
                 {body.left + width / 4, middle_y}, {body.left, body.top});
      break;
    case GateShape::kNot:
    case GateShape::kBuffer:
      MoveToEx(device, body.left, body.top, nullptr);
      LineTo(device, output_x, middle_y);
      LineTo(device, body.left, body.bottom);
      LineTo(device, body.left, body.top);
      break;
    case GateShape::kGeneric:
      RoundRect(device, body.left, body.top, output_x, body.bottom,
                Scale(window, 8), Scale(window, 8));
      break;
  }
  EndPath(device);
  StrokeAndFillPath(device);

  if (appearance.shape == GateShape::kXor) {
    const int offset = Scale(window, 5);
    MoveToEx(device, body.left - offset, body.bottom, nullptr);
    DrawBezier(device, {body.left - offset, body.bottom},
               {body.left + width / 4 - offset, middle_y},
               {body.left + width / 4 - offset, middle_y},
               {body.left - offset, body.top});
  }
  if (appearance.inverted_output) {
    Ellipse(device, body.right - bubble_radius * 2, middle_y - bubble_radius,
            body.right, middle_y + bubble_radius);
  }
  if (appearance.inverted_b_input) {
    const auto inverted_pin = std::find_if(
        layout.pins.begin(), layout.pins.end(), [](const auto& pin) {
          return pin.first->direction ==
                     adapters::NetlistPortDirection::kInput &&
                 pin.first->name == "B";
        });
    if (inverted_pin != layout.pins.end()) {
      const POINT pin = inverted_pin->second;
      int bubble_x = body.left;
      if (appearance.shape == GateShape::kOr) {
        bubble_x = OrInputBoundaryX(window, body, pin.y, false);
      }
      Ellipse(device, bubble_x - bubble_radius, pin.y - bubble_radius,
              bubble_x + bubble_radius, pin.y + bubble_radius);
    }
  }

  RECT gate_name_bounds{layout.bounds.left, body.bottom, layout.bounds.right,
                        layout.bounds.bottom};
  SetTextColor(device, kSchematicBlack);
  DrawCenteredText(device, appearance.label, gate_name_bounds);
  RECT instance_bounds{body.right - Scale(window, 28), layout.bounds.top,
                       layout.bounds.right, body.top};
  SetTextColor(device, kSecondaryText);
  DrawCenteredText(device, layout.display_name, instance_bounds,
                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void DrawPortSymbol(HDC device, HWND window, const PortLayout& layout) {
  RECT bounds = layout.bounds;
  SetTextColor(device, kSchematicBlack);
  if (layout.port->direction == adapters::NetlistPortDirection::kInput) {
    bounds.right -= Scale(window, 8);
    DrawCenteredText(device, Utf8ToWide(layout.label), bounds,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
  } else {
    bounds.left += Scale(window, 8);
    DrawCenteredText(device, Utf8ToWide(layout.label), bounds,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }
}

void DrawPinSquare(HDC device, HWND window, POINT pin, HBRUSH pin_brush) {
  const int radius = Scale(window, 3);
  RECT marker{pin.x - radius, pin.y - radius, pin.x + radius + 1,
              pin.y + radius + 1};
  FillRect(device, &marker, pin_brush);
}

}  // namespace

GateSchematicCanvas::~GateSchematicCanvas() {
  if (window_ != nullptr) DestroyWindow(window_);
}

bool GateSchematicCanvas::Create(HINSTANCE instance, HWND parent) {
  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.hInstance = instance;
  window_class.lpfnWndProc = WindowProcedure;
  window_class.lpszClassName = kCanvasClassName;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = nullptr;
  if (RegisterClassExW(&window_class) == 0 &&
      GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  window_ = CreateWindowExW(WS_EX_CLIENTEDGE, kCanvasClassName, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_HSCROLL |
                                WS_VSCROLL | WS_CLIPSIBLINGS,
                            0, 0, 0, 0, parent, nullptr, instance, this);
  return window_ != nullptr;
}

void GateSchematicCanvas::Move(int x, int y, int width, int height) {
  if (window_ == nullptr) return;
  MoveWindow(window_, x, y, width, height, TRUE);
  UpdateScrollbars();
}

void GateSchematicCanvas::SetSchematic(adapters::GateSchematic schematic) {
  schematic_ = std::move(schematic);
  routing_cache_ = {};
  routing_cache_valid_ = false;
  has_schematic_ = true;
  message_.clear();
  const std::vector<int> logic_depths = CalculateLogicDepths(schematic_);
  int columns = 1;
  std::map<int, int> cells_per_depth;
  for (const int depth : logic_depths) {
    columns = std::max(columns, depth);
    ++cells_per_depth[depth];
  }
  int cell_rows = 1;
  for (const auto& [depth, count] : cells_per_depth) {
    static_cast<void>(depth);
    cell_rows = std::max(cell_rows, count);
  }
  const auto count_port_bits =
      [this](adapters::NetlistPortDirection direction) {
        std::size_t count = 0;
        for (const adapters::NetlistPort& port : schematic_.ports) {
          if (port.direction == direction) count += port.bits.size();
        }
        return static_cast<int>(count);
      };
  const int input_rows =
      count_port_bits(adapters::NetlistPortDirection::kInput);
  const int output_rows =
      count_port_bits(adapters::NetlistPortDirection::kOutput) +
      count_port_bits(adapters::NetlistPortDirection::kInout);
  content_width_ = Scale(window_, 360 + columns * kLogicColumnSpacing);
  const int cell_content_height = 80 + cell_rows * kLogicRowSpacing;
  const int port_content_height = 80 + std::max(input_rows, output_rows) * 40;
  content_height_ =
      Scale(window_, std::max({340, cell_content_height, port_content_height}));
  zoom_ = 1.0;
  scroll_x_ = 0;
  scroll_y_ = 0;
  FitToWindow();
  InvalidateRect(window_, nullptr, TRUE);
}

void GateSchematicCanvas::SetMessage(std::wstring message) {
  schematic_ = {};
  routing_cache_ = {};
  routing_cache_valid_ = false;
  has_schematic_ = false;
  message_ = std::move(message);
  content_width_ = Scale(window_, 800);
  content_height_ = Scale(window_, 500);
  zoom_ = 1.0;
  scroll_x_ = 0;
  scroll_y_ = 0;
  UpdateScrollbars();
  InvalidateRect(window_, nullptr, TRUE);
}

HWND GateSchematicCanvas::Window() const noexcept { return window_; }

LRESULT CALLBACK GateSchematicCanvas::WindowProcedure(HWND window, UINT message,
                                                      WPARAM wparam,
                                                      LPARAM lparam) {
  auto* self = reinterpret_cast<GateSchematicCanvas*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<GateSchematicCanvas*>(create->lpCreateParams);
    self->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self == nullptr ? DefWindowProcW(window, message, wparam, lparam)
                         : self->HandleMessage(message, wparam, lparam);
}

LRESULT GateSchematicCanvas::HandleMessage(UINT message, WPARAM wparam,
                                           LPARAM lparam) {
  switch (message) {
    case WM_PAINT:
      Paint();
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      UpdateScrollbars();
      return 0;
    case WM_LBUTTONDOWN:
      SetFocus(window_);
      return 0;
    case WM_MBUTTONDOWN:
      SetFocus(window_);
      BeginPan({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      return 0;
    case WM_MBUTTONUP:
      EndPan();
      return 0;
    case WM_MOUSEMOVE:
      if (panning_) {
        UpdatePan({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      }
      return 0;
    case WM_CAPTURECHANGED:
      panning_ = false;
      return 0;
    case WM_HSCROLL:
      Scroll(true, LOWORD(wparam), HIWORD(wparam));
      return 0;
    case WM_VSCROLL:
      Scroll(false, LOWORD(wparam), HIWORD(wparam));
      return 0;
    case WM_MOUSEWHEEL: {
      POINT cursor{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(window_, &cursor);
      ZoomAt(GET_WHEEL_DELTA_WPARAM(wparam), cursor);
    }
      return 0;
    case WM_KEYDOWN:
      if (wparam == 'F' || wparam == '0') {
        FitToWindow();
      } else if (wparam == VK_ADD || wparam == VK_OEM_PLUS) {
        RECT client{};
        GetClientRect(window_, &client);
        ZoomAt(WHEEL_DELTA, {client.right / 2, client.bottom / 2});
      } else if (wparam == VK_SUBTRACT || wparam == VK_OEM_MINUS) {
        RECT client{};
        GetClientRect(window_, &client);
        ZoomAt(-WHEEL_DELTA, {client.right / 2, client.bottom / 2});
      } else if (wparam == VK_LEFT || wparam == VK_RIGHT) {
        Scroll(true, wparam == VK_LEFT ? SB_LINEUP : SB_LINEDOWN, 0);
      } else if (wparam == VK_UP || wparam == VK_DOWN) {
        Scroll(false, wparam == VK_UP ? SB_LINEUP : SB_LINEDOWN, 0);
      }
      return 0;
    case WM_NCDESTROY:
      window_ = nullptr;
      return 0;
    default:
      return DefWindowProcW(window_, message, wparam, lparam);
  }
}

void GateSchematicCanvas::Paint() {
  PAINTSTRUCT paint{};
  HDC device = BeginPaint(window_, &paint);
  RECT client{};
  GetClientRect(window_, &client);
  HDC buffer = CreateCompatibleDC(device);
  HBITMAP bitmap = CreateCompatibleBitmap(device, client.right, client.bottom);
  HGDIOBJ old_bitmap = SelectObject(buffer, bitmap);
  HBRUSH background = CreateSolidBrush(kCanvasBackground);
  FillRect(buffer, &client, background);
  DeleteObject(background);
  SetBkMode(buffer, TRANSPARENT);
  SelectObject(buffer, GetStockObject(DEFAULT_GUI_FONT));

  if (!has_schematic_) {
    SetTextColor(buffer, kSecondaryText);
    DrawCenteredText(buffer, message_, client,
                     DT_CENTER | DT_VCENTER | DT_WORDBREAK);
  } else {
    DrawSchematicGrid(buffer, window_, client, zoom_, scroll_x_, scroll_y_);
    SetGraphicsMode(buffer, GM_ADVANCED);
    XFORM transform{static_cast<FLOAT>(zoom_),
                    0.0F,
                    0.0F,
                    static_cast<FLOAT>(zoom_),
                    static_cast<FLOAT>(-scroll_x_),
                    static_cast<FLOAT>(-scroll_y_)};
    SetWorldTransform(buffer, &transform);
    constexpr int offset_x = 0;
    constexpr int offset_y = 0;
    const int title_height = Scale(window_, 52);
    RECT title{offset_x + Scale(window_, 24), offset_y + Scale(window_, 12),
               offset_x + content_width_ - Scale(window_, 24),
               offset_y + title_height};
    SetTextColor(buffer, kSchematicBlack);
    DrawCenteredText(
        buffer, L"Gate-Level Schematic — " + Utf8ToWide(schematic_.top_module),
        title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const int port_width = Scale(window_, 82);
    const int port_height = Scale(window_, 24);
    const int cell_width = Scale(window_, 76);
    const int cell_gap_x = Scale(window_, kLogicColumnSpacing);
    const int cell_gap_y = Scale(window_, kLogicRowSpacing);
    const int cell_start_x = offset_x + Scale(window_, kFirstLogicColumnX);
    const int start_y = offset_y + Scale(window_, 68);
    const int output_x = offset_x + content_width_ - Scale(window_, 102);
    std::map<std::string, NetRoute> nets;
    std::map<std::string, int> input_net_order;
    std::vector<PortLayout> port_layouts;
    int input_index = 0;
    int output_index = 0;
    for (const adapters::NetlistPort& port : schematic_.ports) {
      const bool input =
          port.direction == adapters::NetlistPortDirection::kInput;
      for (std::size_t bit_index = 0; bit_index < port.bits.size();
           ++bit_index) {
        const int x = input ? offset_x + Scale(window_, 28) : output_x;
        const int index = input ? input_index++ : output_index++;
        const int port_y = start_y + Scale(window_, 29) + index * cell_gap_y;
        RECT bounds{x, port_y, x + port_width, port_y + port_height};
        std::string label = port.name;
        if (port.bits.size() > 1U) {
          label += "[" + std::to_string(bit_index) + "]";
        }
        const std::string& bit = port.bits[bit_index];
        port_layouts.push_back({&port, std::move(label), bit, bounds});
        const POINT endpoint =
            input ? POINT{bounds.right, (bounds.top + bounds.bottom) / 2}
                  : POINT{bounds.left, (bounds.top + bounds.bottom) / 2};
        if (input) {
          nets[bit].driver = endpoint;
          input_net_order.insert_or_assign(bit, index);
        } else {
          nets[bit].sinks.push_back(endpoint);
        }
      }
    }

    struct OrderedCell {
      const adapters::NetlistCell* cell = nullptr;
      std::size_t original_index = 0;
      int depth = 1;
      int vertical_rank = 1'000'000;
    };
    const std::vector<int> logic_depths = CalculateLogicDepths(schematic_);
    const std::vector<int> vertical_ranks =
        CalculateVerticalRanks(schematic_, logic_depths);

    std::vector<OrderedCell> ordered_cells;
    ordered_cells.reserve(schematic_.cells.size());
    for (std::size_t index = 0; index < schematic_.cells.size(); ++index) {
      ordered_cells.push_back({&schematic_.cells[index], index,
                               logic_depths[index], vertical_ranks[index]});
    }
    std::stable_sort(ordered_cells.begin(), ordered_cells.end(),
                     [](const OrderedCell& left, const OrderedCell& right) {
                       if (left.depth != right.depth) {
                         return left.depth < right.depth;
                       }
                       return left.vertical_rank < right.vertical_rank;
                     });
    std::vector<NodeLayout> cells;
    cells.reserve(ordered_cells.size());
    std::map<int, int> cells_by_depth;
    int maximum_rows_in_level = 1;
    for (const OrderedCell& ordered : ordered_cells) {
      const int count = ++cells_by_depth[ordered.depth];
      maximum_rows_in_level = std::max(maximum_rows_in_level, count);
    }
    std::map<int, int> next_row_by_depth;
    for (std::size_t index = 0; index < ordered_cells.size(); ++index) {
      const OrderedCell& ordered = ordered_cells[index];
      const adapters::NetlistCell& cell = *ordered.cell;
      const int column = ordered.depth - 1;
      const int row = next_row_by_depth[ordered.depth]++;
      const int level_top =
          ((maximum_rows_in_level - cells_by_depth[ordered.depth]) *
           cell_gap_y) /
          2;
      const std::size_t input_pins = static_cast<std::size_t>(std::count_if(
          cell.ports.begin(), cell.ports.end(), [](const auto& port) {
            return port.direction == adapters::NetlistPortDirection::kInput;
          }));
      const int pin_count = static_cast<int>(std::max<std::size_t>(
          2U, std::max(input_pins, cell.ports.size() - input_pins)));
      const int cell_height =
          std::max(Scale(window_, 84), Scale(window_, 34 + pin_count * 16));
      NodeLayout layout;
      layout.cell = &cell;
      layout.display_name = InstanceDisplayName(ordered.original_index);
      layout.bounds = {cell_start_x + column * cell_gap_x,
                       start_y + level_top + row * cell_gap_y,
                       cell_start_x + column * cell_gap_x + cell_width,
                       start_y + level_top + row * cell_gap_y + cell_height};
      int input_pin = 0;
      int output_pin = 0;
      const RECT body = GateBody(window_, layout.bounds);
      const int body_top = body.top;
      const int body_bottom = body.bottom;
      const int body_height = body_bottom - body_top;
      const int output_pins = static_cast<int>(cell.ports.size() - input_pins);
      std::vector<const adapters::NetlistCellPort*> ordered_ports;
      ordered_ports.reserve(cell.ports.size());
      for (const adapters::NetlistCellPort& port : cell.ports) {
        ordered_ports.push_back(&port);
      }
      std::stable_sort(
          ordered_ports.begin(), ordered_ports.end(),
          [&input_net_order](const auto* left, const auto* right) {
            const auto group = [](const auto* port) {
              if (port->direction == adapters::NetlistPortDirection::kInput) {
                return 0;
              }
              return port->direction == adapters::NetlistPortDirection::kInout
                         ? 1
                         : 2;
            };
            if (group(left) != group(right)) {
              return group(left) < group(right);
            }
            const auto rank = [&input_net_order](const auto* port) {
              int result = 1'000'000;
              for (const std::string& bit : port->bits) {
                const auto found = input_net_order.find(bit);
                if (found != input_net_order.end()) {
                  result = std::min(result, found->second);
                }
              }
              return result;
            };
            return rank(left) < rank(right);
          });
      for (const adapters::NetlistCellPort* port : ordered_ports) {
        const bool input =
            port->direction == adapters::NetlistPortDirection::kInput;
        const int pin_index = input ? input_pin++ : output_pin++;
        const int pin_total =
            input ? static_cast<int>(input_pins) : output_pins;
        const int pin_y =
            body_top + ((pin_index + 1) * body_height) / (pin_total + 1);
        const POINT endpoint{input ? layout.bounds.left : layout.bounds.right,
                             pin_y};
        layout.pins.emplace_back(port, endpoint);
        for (const std::string& bit : port->bits) {
          if (input) {
            nets[bit].sinks.push_back(endpoint);
          } else {
            nets[bit].driver = endpoint;
          }
        }
      }
      cells.push_back(std::move(layout));
    }

    const auto align_ports_to_connected_logic =
        [this, port_height, start_y, &nets](std::vector<PortLayout*> layouts,
                                            bool inputs) {
          struct PortTarget {
            PortLayout* layout = nullptr;
            int center_y = 0;
          };
          std::vector<PortTarget> targets;
          targets.reserve(layouts.size());
          for (PortLayout* layout : layouts) {
            const auto route = nets.find(layout->bit);
            int center_y = (layout->bounds.top + layout->bounds.bottom) / 2;
            if (route != nets.end()) {
              if (inputs && !route->second.sinks.empty()) {
                long long total_y = 0;
                for (const POINT sink : route->second.sinks) {
                  total_y += sink.y;
                }
                center_y =
                    static_cast<int>(total_y / static_cast<long long>(
                                                   route->second.sinks.size()));
              } else if (!inputs && route->second.driver) {
                center_y = route->second.driver->y;
              }
            }
            targets.push_back({layout, center_y});
          }
          std::stable_sort(targets.begin(), targets.end(),
                           [](const PortTarget& left, const PortTarget& right) {
                             return left.center_y < right.center_y;
                           });
          const int minimum_gap = Scale(window_, 40);
          int previous_y = start_y - minimum_gap;
          for (PortTarget& target : targets) {
            target.center_y =
                std::max(target.center_y, previous_y + minimum_gap);
            previous_y = target.center_y;
          }
          const int maximum_y = content_height_ - Scale(window_, 36);
          if (!targets.empty() && targets.back().center_y > maximum_y) {
            const int shift = targets.back().center_y - maximum_y;
            for (PortTarget& target : targets) target.center_y -= shift;
          }
          for (const PortTarget& target : targets) {
            PortLayout& layout = *target.layout;
            const POINT old_endpoint =
                inputs ? POINT{layout.bounds.right,
                               (layout.bounds.top + layout.bounds.bottom) / 2}
                       : POINT{layout.bounds.left,
                               (layout.bounds.top + layout.bounds.bottom) / 2};
            layout.bounds.top = target.center_y - port_height / 2;
            layout.bounds.bottom = layout.bounds.top + port_height;
            const POINT new_endpoint =
                inputs ? POINT{layout.bounds.right, target.center_y}
                       : POINT{layout.bounds.left, target.center_y};
            NetRoute& route = nets[layout.bit];
            if (inputs) {
              route.driver = new_endpoint;
            } else {
              const auto endpoint = std::find_if(
                  route.sinks.begin(), route.sinks.end(),
                  [&old_endpoint](POINT sink) {
                    return sink.x == old_endpoint.x && sink.y == old_endpoint.y;
                  });
              if (endpoint != route.sinks.end()) *endpoint = new_endpoint;
            }
          }
        };
    std::vector<PortLayout*> input_port_layouts;
    std::vector<PortLayout*> output_port_layouts;
    for (PortLayout& layout : port_layouts) {
      if (layout.port->direction == adapters::NetlistPortDirection::kInput) {
        input_port_layouts.push_back(&layout);
      } else {
        output_port_layouts.push_back(&layout);
      }
    }
    align_ports_to_connected_logic(std::move(input_port_layouts), true);
    align_ports_to_connected_logic(std::move(output_port_layouts), false);

    HPEN wire_pen = CreatePen(PS_SOLID, Scale(window_, 1), kSchematicBlack);
    HGDIOBJ old_pen = SelectObject(buffer, wire_pen);
    if (!routing_cache_valid_) {
      OrthogonalRoutingRequest routing_request;
      routing_request.width = content_width_;
      routing_request.height = content_height_;
      routing_request.grid = Scale(window_, 8);
      routing_request.level_spacing = cell_gap_x;
      routing_request.obstacle_clearance = Scale(window_, 8);
      routing_request.obstacles.reserve(cells.size());
      for (const NodeLayout& cell : cells) {
        routing_request.obstacles.push_back({cell.bounds.left, cell.bounds.top,
                                             cell.bounds.right,
                                             cell.bounds.bottom});
      }
      routing_request.nets.reserve(nets.size());
      for (const auto& [bit, route] : nets) {
        if (!route.driver || route.sinks.empty() || bit.starts_with("const:")) {
          continue;
        }
        OrthogonalNet net;
        net.id = bit;
        net.source = {route.driver->x, route.driver->y};
        net.sinks.reserve(route.sinks.size());
        for (const POINT sink : route.sinks) {
          net.sinks.push_back({sink.x, sink.y});
        }
        routing_request.nets.push_back(std::move(net));
      }
      routing_cache_ = OrthogonalEdgeRouter().Route(routing_request);
      routing_cache_valid_ = true;
    }
    const OrthogonalRoutingResult& routing_result = routing_cache_;
    const int junction_radius =
        std::max(1, static_cast<int>(std::lround(Scale(window_, 2) / zoom_)));
    const auto is_junction = [&routing_result](OrthogonalPoint point) {
      return std::find(routing_result.junctions.begin(),
                       routing_result.junctions.end(),
                       point) != routing_result.junctions.end();
    };
    const auto trim_from_junction = [junction_radius](OrthogonalPoint point,
                                                      OrthogonalPoint toward) {
      if (point.x < toward.x) point.x += junction_radius;
      if (point.x > toward.x) point.x -= junction_radius;
      if (point.y < toward.y) point.y += junction_radius;
      if (point.y > toward.y) point.y -= junction_radius;
      return point;
    };
    for (const OrthogonalRoute& route : routing_result.routes) {
      for (std::size_t index = 1; index < route.points.size(); ++index) {
        OrthogonalPoint start = route.points[index - 1];
        OrthogonalPoint end = route.points[index];
        if (is_junction(start)) start = trim_from_junction(start, end);
        if (is_junction(end)) end = trim_from_junction(end, start);
        MoveToEx(buffer, start.x, start.y, nullptr);
        LineTo(buffer, end.x, end.y);
      }
    }
    const int bridge_radius = Scale(window_, 5);
    HPEN bridge_gap_pen =
        CreatePen(PS_SOLID, Scale(window_, 3), kCanvasBackground);
    for (const OrthogonalBridge& bridge : routing_result.bridges) {
      if (!bridge.horizontal_over) continue;
      SelectObject(buffer, bridge_gap_pen);
      MoveToEx(buffer, bridge.point.x - bridge_radius, bridge.point.y, nullptr);
      LineTo(buffer, bridge.point.x + bridge_radius, bridge.point.y);

      SelectObject(buffer, wire_pen);
      MoveToEx(buffer, bridge.point.x, bridge.point.y - bridge_radius, nullptr);
      LineTo(buffer, bridge.point.x, bridge.point.y + bridge_radius);
      const POINT arc[] = {
          {bridge.point.x - bridge_radius, bridge.point.y},
          {bridge.point.x - bridge_radius / 2, bridge.point.y - bridge_radius},
          {bridge.point.x + bridge_radius / 2, bridge.point.y - bridge_radius},
          {bridge.point.x + bridge_radius, bridge.point.y}};
      PolyBezier(buffer, arc, static_cast<DWORD>(std::size(arc)));
    }
    DeleteObject(bridge_gap_pen);
    HPEN junction_pen = CreatePen(PS_SOLID, Scale(window_, 1), kJunctionBlue);
    HBRUSH junction_brush = CreateSolidBrush(kJunctionBlue);
    SelectObject(buffer, junction_pen);
    HGDIOBJ old_brush = SelectObject(buffer, junction_brush);
    for (const OrthogonalPoint junction : routing_result.junctions) {
      Ellipse(buffer, junction.x - junction_radius,
              junction.y - junction_radius, junction.x + junction_radius + 1,
              junction.y + junction_radius + 1);
    }
    SelectObject(buffer, old_brush);
    SelectObject(buffer, old_pen);
    DeleteObject(junction_brush);
    DeleteObject(junction_pen);
    DeleteObject(wire_pen);

    HBRUSH port_brush = CreateSolidBrush(kCanvasBackground);
    HBRUSH cell_brush = CreateSolidBrush(kCanvasBackground);
    HBRUSH pin_brush = CreateSolidBrush(kPinRed);
    HPEN border_pen = CreatePen(PS_SOLID, Scale(window_, 1), kSchematicBlack);
    old_pen = SelectObject(buffer, border_pen);
    old_brush = SelectObject(buffer, port_brush);
    for (const PortLayout& layout : port_layouts) {
      DrawPortSymbol(buffer, window_, layout);
      const POINT pin =
          layout.port->direction == adapters::NetlistPortDirection::kInput
              ? POINT{layout.bounds.right,
                      (layout.bounds.top + layout.bounds.bottom) / 2}
              : POINT{layout.bounds.left,
                      (layout.bounds.top + layout.bounds.bottom) / 2};
      DrawPinSquare(buffer, window_, pin, pin_brush);
    }
    SelectObject(buffer, cell_brush);
    for (const NodeLayout& layout : cells) {
      const GateAppearance appearance = GetGateAppearance(layout.cell->type);
      const RECT body = GateBody(window_, layout.bounds);
      const auto pin_body_x = [this, &appearance, &body](
                                  const adapters::NetlistCellPort* port,
                                  POINT endpoint) {
        const bool input =
            port->direction == adapters::NetlistPortDirection::kInput;
        int body_x = input ? body.left : body.right;
        if (input && (appearance.shape == GateShape::kOr ||
                      appearance.shape == GateShape::kXor)) {
          body_x = OrInputBoundaryX(window_, body, endpoint.y,
                                    appearance.shape == GateShape::kXor);
        }
        return body_x;
      };
      for (const auto& [port, endpoint] : layout.pins) {
        MoveToEx(buffer, endpoint.x, endpoint.y, nullptr);
        LineTo(buffer, pin_body_x(port, endpoint), endpoint.y);
      }
      DrawGateSymbol(buffer, window_, layout);
      for (const auto& [port, endpoint] : layout.pins) {
        const bool input =
            port->direction == adapters::NetlistPortDirection::kInput;
        if (appearance.shape == GateShape::kGeneric) {
          RECT label{
              body.left + Scale(window_, 3), endpoint.y - Scale(window_, 8),
              body.left + Scale(window_, 35), endpoint.y + Scale(window_, 8)};
          UINT label_format = DT_LEFT | DT_VCENTER | DT_SINGLELINE;
          if (!input) {
            label = {
                body.right - Scale(window_, 35), endpoint.y - Scale(window_, 8),
                body.right - Scale(window_, 3), endpoint.y + Scale(window_, 8)};
            label_format = DT_RIGHT | DT_VCENTER | DT_SINGLELINE;
          }
          SetTextColor(buffer, kSchematicBlack);
          DrawCenteredText(buffer, Utf8ToWide(port->name), label, label_format);
        }
        DrawPinSquare(buffer, window_, endpoint, pin_brush);
      }
    }
    SelectObject(buffer, old_brush);
    SelectObject(buffer, old_pen);
    DeleteObject(border_pen);
    DeleteObject(pin_brush);
    DeleteObject(port_brush);
    DeleteObject(cell_brush);

    ModifyWorldTransform(buffer, nullptr, MWT_IDENTITY);
    SetGraphicsMode(buffer, GM_COMPATIBLE);
    const std::wstring navigation =
        std::to_wstring(static_cast<int>(zoom_ * 100.0 + 0.5)) +
        L"%   Wheel: Zoom   Middle-drag: Pan   F: Fit";
    RECT navigation_bounds{Scale(window_, 12),
                           client.bottom - Scale(window_, 30),
                           client.right - Scale(window_, 12), client.bottom};
    HBRUSH navigation_background = CreateSolidBrush(kCanvasBackground);
    FillRect(buffer, &navigation_bounds, navigation_background);
    DeleteObject(navigation_background);
    SetTextColor(buffer, kSecondaryText);
    DrawCenteredText(buffer, navigation, navigation_bounds,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }

  BitBlt(device, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
  SelectObject(buffer, old_bitmap);
  DeleteObject(bitmap);
  DeleteDC(buffer);
  EndPaint(window_, &paint);
}

void GateSchematicCanvas::UpdateScrollbars() {
  if (window_ == nullptr) return;
  RECT client{};
  GetClientRect(window_, &client);
  SCROLLINFO horizontal{
      sizeof(horizontal),
      SIF_ALL,
      0,
      std::max(0, static_cast<int>(content_width_ * zoom_) - 1),
      static_cast<UINT>(std::max(0L, client.right)),
      scroll_x_,
      0};
  scroll_x_ = SetScrollInfo(window_, SB_HORZ, &horizontal, TRUE);
  SCROLLINFO vertical{
      sizeof(vertical),
      SIF_ALL,
      0,
      std::max(0, static_cast<int>(content_height_ * zoom_) - 1),
      static_cast<UINT>(std::max(0L, client.bottom)),
      scroll_y_,
      0};
  scroll_y_ = SetScrollInfo(window_, SB_VERT, &vertical, TRUE);
}

void GateSchematicCanvas::Scroll(bool horizontal, int command, int position) {
  static_cast<void>(position);
  const int bar = horizontal ? SB_HORZ : SB_VERT;
  SCROLLINFO info{sizeof(info), SIF_ALL};
  GetScrollInfo(window_, bar, &info);
  int next = info.nPos;
  const int line = Scale(window_, 36);
  if (command == SB_LINEUP) next -= line;
  if (command == SB_LINEDOWN) next += line;
  if (command == SB_PAGEUP) next -= static_cast<int>(info.nPage);
  if (command == SB_PAGEDOWN) next += static_cast<int>(info.nPage);
  if (command == SB_THUMBTRACK || command == SB_THUMBPOSITION) {
    next = info.nTrackPos;
  }
  next = std::clamp(
      next, info.nMin,
      std::max(info.nMin, info.nMax - static_cast<int>(info.nPage) + 1));
  if (horizontal) {
    scroll_x_ = next;
  } else {
    scroll_y_ = next;
  }
  info.fMask = SIF_POS;
  info.nPos = next;
  SetScrollInfo(window_, bar, &info, TRUE);
  InvalidateRect(window_, nullptr, TRUE);
}

void GateSchematicCanvas::ZoomAt(int wheel_delta, POINT cursor) {
  if (!has_schematic_ || wheel_delta == 0) return;
  const double old_zoom = zoom_;
  const double steps = static_cast<double>(wheel_delta) / WHEEL_DELTA;
  zoom_ = std::clamp(old_zoom * std::pow(1.15, steps), 0.25, 4.0);
  const double logical_x = (scroll_x_ + cursor.x) / old_zoom;
  const double logical_y = (scroll_y_ + cursor.y) / old_zoom;
  scroll_x_ = static_cast<int>(logical_x * zoom_ - cursor.x);
  scroll_y_ = static_cast<int>(logical_y * zoom_ - cursor.y);
  UpdateScrollbars();
  InvalidateRect(window_, nullptr, TRUE);
}

void GateSchematicCanvas::FitToWindow() {
  if (window_ == nullptr || !has_schematic_) {
    UpdateScrollbars();
    return;
  }
  RECT client{};
  GetClientRect(window_, &client);
  if (client.right <= 0 || client.bottom <= 0 || content_width_ <= 0 ||
      content_height_ <= 0) {
    return;
  }
  const double horizontal =
      static_cast<double>(std::max(1L, client.right - Scale(window_, 28))) /
      content_width_;
  const double vertical =
      static_cast<double>(std::max(1L, client.bottom - Scale(window_, 48))) /
      content_height_;
  zoom_ = std::clamp(std::min(horizontal, vertical), 0.25, 2.0);
  scroll_x_ = 0;
  scroll_y_ = 0;
  UpdateScrollbars();
  InvalidateRect(window_, nullptr, TRUE);
}

void GateSchematicCanvas::BeginPan(POINT cursor) {
  panning_ = true;
  pan_origin_ = cursor;
  pan_scroll_x_ = scroll_x_;
  pan_scroll_y_ = scroll_y_;
  SetCapture(window_);
  SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
}

void GateSchematicCanvas::UpdatePan(POINT cursor) {
  if (!panning_) return;
  scroll_x_ = pan_scroll_x_ - (cursor.x - pan_origin_.x);
  scroll_y_ = pan_scroll_y_ - (cursor.y - pan_origin_.y);
  UpdateScrollbars();
  InvalidateRect(window_, nullptr, TRUE);
}

void GateSchematicCanvas::EndPan() {
  if (!panning_) return;
  panning_ = false;
  if (GetCapture() == window_) ReleaseCapture();
  SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

}  // namespace designpp::gui
