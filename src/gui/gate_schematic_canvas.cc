// Copyright 2026 The Design++ Authors

#include "designpp/gui/gate_schematic_canvas.h"

#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <string_view>
#include <utility>

#include "designpp/gui/schematic_viewport.h"

namespace designpp::gui {
namespace {

constexpr wchar_t kCanvasClassName[] = L"DesignPlusPlus.GateSchematicCanvas";
constexpr COLORREF kCanvasBackground = RGB(255, 255, 255);
constexpr COLORREF kGridMinorLine = RGB(236, 236, 236);
constexpr COLORREF kGridMajorLine = RGB(227, 227, 227);
constexpr COLORREF kSchematicBlack = RGB(24, 24, 24);
constexpr COLORREF kSecondaryText = RGB(96, 96, 96);
constexpr COLORREF kPinRed = RGB(196, 32, 38);
constexpr COLORREF kJunctionBlue = RGB(0, 102, 204);
constexpr double kMinimumZoom = 0.001;
constexpr double kMaximumZoom = 4.0;

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

void DrawCenteredText(HDC device, const std::wstring& text, RECT bounds,
                      UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE |
                                    DT_END_ELLIPSIS) {
  DrawTextW(device, text.c_str(), static_cast<int>(text.size()), &bounds,
            format | DT_NOPREFIX);
}

void DrawGrid(HDC device, HWND window, RECT client, double zoom, int scroll_x,
              int scroll_y) {
  const double spacing = Scale(window, 8) * zoom;
  int step = 1;
  while (spacing * step < Scale(window, 8)) step *= 2;
  const int first_x = static_cast<int>(std::floor(scroll_x / spacing));
  const int last_x =
      static_cast<int>(std::ceil((scroll_x + client.right) / spacing));
  const int first_y = static_cast<int>(std::floor(scroll_y / spacing));
  const int last_y =
      static_cast<int>(std::ceil((scroll_y + client.bottom) / spacing));
  HPEN minor = CreatePen(PS_SOLID, 1, kGridMinorLine);
  HPEN major = CreatePen(PS_SOLID, 1, kGridMajorLine);
  HGDIOBJ old_pen = SelectObject(device, minor);
  for (int column = first_x; column <= last_x; ++column) {
    if (column % step != 0) continue;
    SelectObject(device, column % 5 == 0 ? major : minor);
    const int x = static_cast<int>(std::lround(column * spacing)) - scroll_x;
    MoveToEx(device, x, client.top, nullptr);
    LineTo(device, x, client.bottom);
  }
  for (int row = first_y; row <= last_y; ++row) {
    if (row % step != 0) continue;
    SelectObject(device, row % 5 == 0 ? major : minor);
    const int y = static_cast<int>(std::lround(row * spacing)) - scroll_y;
    MoveToEx(device, client.left, y, nullptr);
    LineTo(device, client.right, y);
  }
  SelectObject(device, old_pen);
  DeleteObject(major);
  DeleteObject(minor);
}

RECT ToRect(OrthogonalRect rect) {
  return {rect.left, rect.top, rect.right, rect.bottom};
}

RECT NodeBody(const SchematicSceneNode& node) {
  RECT body = ToRect(node.bounds);
  body.left += 8;
  body.right -= 8;
  body.top += 16;
  body.bottom -= 18;
  return body;
}

bool TypeContains(std::string_view type, std::string_view value) {
  return std::search(type.begin(), type.end(), value.begin(), value.end(),
                     [](char left, char right) {
                       return std::toupper(static_cast<unsigned char>(left)) ==
                              std::toupper(static_cast<unsigned char>(right));
                     }) != type.end();
}

bool IsXorFamily(std::string_view type) {
  return TypeContains(type, "XOR") || TypeContains(type, "XNOR");
}

bool IsOrFamily(std::string_view type) {
  return !IsXorFamily(type) &&
         (TypeContains(type, "OR") || TypeContains(type, "NOR"));
}

bool HasConcaveInputFace(std::string_view type) {
  return IsOrFamily(type) || IsXorFamily(type);
}

bool HasInvertedOutput(std::string_view type) {
  return TypeContains(type, "NAND") || TypeContains(type, "NOR") ||
         TypeContains(type, "XNOR") || TypeContains(type, "NOT");
}

int ConcaveInputBoundaryX(RECT body, int pin_y) {
  constexpr int kSamples = 64;
  const double width = static_cast<double>(body.right - body.left);
  const double middle_y = static_cast<double>(body.top + body.bottom) / 2.0;
  double best_distance = std::numeric_limits<double>::max();
  double best_x = static_cast<double>(body.left);
  for (int sample = 0; sample <= kSamples; ++sample) {
    const double t = static_cast<double>(sample) / kSamples;
    const double inverse = 1.0 - t;
    const double first = inverse * inverse * inverse;
    const double second = 3.0 * inverse * inverse * t;
    const double third = 3.0 * inverse * t * t;
    const double fourth = t * t * t;
    const double x = first * body.left +
                     (second + third) * (body.left + width / 4.0) +
                     fourth * body.left;
    const double y =
        first * body.bottom + (second + third) * middle_y + fourth * body.top;
    const double distance = std::abs(y - pin_y);
    if (distance < best_distance) {
      best_distance = distance;
      best_x = x;
    }
  }
  return static_cast<int>(std::lround(best_x));
}

int PrimitiveInputBoundaryX(const SchematicSceneNode& node, RECT body,
                            int pin_y) {
  if (!HasConcaveInputFace(node.type)) return body.left;
  const int boundary_x = ConcaveInputBoundaryX(body, pin_y);
  return IsXorFamily(node.type) ? boundary_x - 5 : boundary_x;
}

int PrimitiveInputLabelX(const SchematicSceneNode& node, RECT body, int pin_y) {
  return HasConcaveInputFace(node.type) ? ConcaveInputBoundaryX(body, pin_y)
                                        : body.left;
}

std::wstring CompactType(std::string_view type) {
  std::string result;
  for (const char value : type) {
    if (value != '$' && value != '_') {
      result.push_back(
          static_cast<char>(std::toupper(static_cast<unsigned char>(value))));
    }
  }
  return Utf8ToWide(result);
}

void DrawBezierTo(HDC device, POINT first, POINT second, POINT end) {
  POINT points[]{first, second, end};
  PolyBezierTo(device, points, 3);
}

HFONT CreateSchematicFont(int height, int weight = FW_NORMAL) {
  return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

void DrawPrimitive(HDC device, const SchematicSceneNode& node, RECT body) {
  const bool inverted = HasInvertedOutput(node.type);
  const bool is_xor = IsXorFamily(node.type);
  const bool is_or = IsOrFamily(node.type);
  const bool is_not =
      TypeContains(node.type, "NOT") || TypeContains(node.type, "BUF");
  const int middle_y = (body.top + body.bottom) / 2;
  const int bubble = 3;
  const int output_x = inverted ? body.right - bubble * 2 : body.right;
  const int width = body.right - body.left;
  const int height = body.bottom - body.top;
  BeginPath(device);
  if (is_not) {
    MoveToEx(device, body.left, body.top, nullptr);
    LineTo(device, output_x, middle_y);
    LineTo(device, body.left, body.bottom);
    LineTo(device, body.left, body.top);
  } else if (is_or || is_xor) {
    MoveToEx(device, body.left, body.top, nullptr);
    DrawBezierTo(device, {body.left + width / 2, body.top},
                 {output_x - width / 5, body.top + height / 5},
                 {output_x, middle_y});
    DrawBezierTo(device, {output_x - width / 5, body.bottom - height / 5},
                 {body.left + width / 2, body.bottom},
                 {body.left, body.bottom});
    DrawBezierTo(device, {body.left + width / 4, middle_y},
                 {body.left + width / 4, middle_y}, {body.left, body.top});
  } else {
    MoveToEx(device, body.left, body.top, nullptr);
    LineTo(device, body.left + width / 2, body.top);
    DrawBezierTo(device, {body.left + width * 3 / 4, body.top},
                 {output_x, body.top + height / 4}, {output_x, middle_y});
    DrawBezierTo(device, {output_x, body.bottom - height / 4},
                 {body.left + width * 3 / 4, body.bottom},
                 {body.left + width / 2, body.bottom});
    LineTo(device, body.left, body.bottom);
    LineTo(device, body.left, body.top);
  }
  EndPath(device);
  StrokeAndFillPath(device);
  if (is_xor) {
    MoveToEx(device, body.left - 5, body.bottom, nullptr);
    DrawBezierTo(device, {body.left + width / 4 - 5, middle_y},
                 {body.left + width / 4 - 5, middle_y},
                 {body.left - 5, body.top});
  }
  if (inverted) {
    Ellipse(device, body.right - bubble * 2, middle_y - bubble, body.right,
            middle_y + bubble);
  }
}

void DrawRegister(HDC device, const SchematicSceneNode& node, RECT body) {
  Rectangle(device, body.left, body.top, body.right, body.bottom);
  for (const SchematicScenePin& pin : node.pins) {
    if (pin.role == core::SchematicPinRole::kClock) {
      POINT clock[]{{body.left, pin.point.y - 5},
                    {body.left + 7, pin.point.y},
                    {body.left, pin.point.y + 5}};
      Polyline(device, clock, static_cast<int>(std::size(clock)));
    }
    if (pin.active_low && pin.direction == core::SchematicDirection::kInput) {
      Ellipse(device, body.left - 3, pin.point.y - 3, body.left + 3,
              pin.point.y + 3);
    }
  }
}

void DrawFunctionalBlock(HDC device, const SchematicSceneNode& node, RECT body,
                         bool show_label) {
  if (node.kind == core::SchematicNodeKind::kMux) {
    POINT mux[]{{body.left, body.top + 4},
                {body.right, body.top},
                {body.right, body.bottom},
                {body.left, body.bottom - 4},
                {body.left, body.top + 4}};
    Polygon(device, mux, static_cast<int>(std::size(mux)));
  } else {
    RoundRect(device, body.left, body.top, body.right, body.bottom, 6, 6);
  }
  if (show_label) {
    SetTextColor(device, kSchematicBlack);
    DrawCenteredText(device, Utf8ToWide(node.label), body);
  }
}

void DrawPinSquare(HDC device, OrthogonalPoint point, HBRUSH brush) {
  RECT marker{point.x - 3, point.y - 3, point.x + 4, point.y + 4};
  FillRect(device, &marker, brush);
}

bool IsConstantTerminal(const SchematicSceneTerminal& terminal) {
  return !terminal.bits.empty() &&
         std::all_of(
             terminal.bits.begin(), terminal.bits.end(),
             [](const std::string& bit) { return bit.starts_with("const:"); });
}

void DrawNode(HDC device, const SchematicSceneNode& node,
              SchematicDetail detail, HBRUSH pin_brush, HFONT pin_font,
              HFONT label_font, HFONT instance_font) {
  const RECT body = NodeBody(node);
  if (detail == SchematicDetail::kOverview) {
    Rectangle(device, body.left, body.top, body.right, body.bottom);
    return;
  }
  SelectObject(device, pin_font);
  for (const SchematicScenePin& pin : node.pins) {
    int body_x = pin.direction == core::SchematicDirection::kInput ? body.left
                                                                   : body.right;
    if (node.kind == core::SchematicNodeKind::kPrimitive &&
        pin.direction == core::SchematicDirection::kInput) {
      body_x = PrimitiveInputBoundaryX(node, body, pin.point.y);
    }
    MoveToEx(device, pin.point.x, pin.point.y, nullptr);
    LineTo(device, body_x, pin.point.y);
  }
  if (node.kind == core::SchematicNodeKind::kPrimitive) {
    DrawPrimitive(device, node, body);
  } else if (node.kind == core::SchematicNodeKind::kRegister ||
             node.kind == core::SchematicNodeKind::kRegisterBank) {
    DrawRegister(device, node, body);
    if (detail == SchematicDetail::kFull) {
      RECT register_name{body.left + 8, body.top + 3, body.right - 8,
                         body.top + 15};
      DrawCenteredText(device, Utf8ToWide(node.label), register_name);
    }
  } else {
    DrawFunctionalBlock(device, node, body, detail == SchematicDetail::kFull);
  }
  for (const SchematicScenePin& pin : node.pins) {
    DrawPinSquare(device, pin.point, pin_brush);
    if (detail != SchematicDetail::kFull) continue;
    RECT label{body.left + 12, pin.point.y - 6, body.left + 42,
               pin.point.y + 6};
    UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE;
    if (node.kind == core::SchematicNodeKind::kPrimitive) {
      if (pin.direction == core::SchematicDirection::kInput) {
        const int boundary_x = PrimitiveInputLabelX(node, body, pin.point.y);
        label = {boundary_x + 4, pin.point.y - 6, boundary_x + 22,
                 pin.point.y + 6};
      } else {
        const int output_x =
            HasInvertedOutput(node.type) ? body.right - 6 : body.right;
        label = {output_x - 22, pin.point.y - 6, output_x - 4, pin.point.y + 6};
        format = DT_RIGHT | DT_VCENTER | DT_SINGLELINE;
      }
    } else if (pin.direction != core::SchematicDirection::kInput) {
      label = {body.right - 27, pin.point.y - 6, body.right - 7,
               pin.point.y + 6};
      format = DT_RIGHT | DT_VCENTER | DT_SINGLELINE;
    }
    DrawCenteredText(device, Utf8ToWide(pin.name), label, format);
  }
  if (detail != SchematicDetail::kFull) return;
  RECT name_bounds{node.bounds.left, body.bottom, node.bounds.right,
                   node.bounds.bottom};
  SelectObject(device, label_font);
  if (node.kind == core::SchematicNodeKind::kPrimitive) {
    DrawCenteredText(device, Utf8ToWide(node.label), name_bounds);
  } else if (node.kind == core::SchematicNodeKind::kRegister ||
             node.kind == core::SchematicNodeKind::kRegisterBank) {
    DrawCenteredText(device, CompactType(node.type), name_bounds);
  }
  RECT instance{body.right - 36, node.bounds.top, node.bounds.right, body.top};
  SelectObject(device, instance_font);
  SetTextColor(device, kSecondaryText);
  DrawCenteredText(device, Utf8ToWide(node.instance_label), instance,
                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
  SetTextColor(device, kSchematicBlack);
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
  window_class.style = CS_DBLCLKS;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
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

void GateSchematicCanvas::SetScene(
    std::shared_ptr<const SchematicScene> scene) {
  scene_ = std::move(scene);
  has_schematic_ = scene_ != nullptr;
  message_.clear();
  content_width_ = has_schematic_ ? scene_->width : 800;
  content_height_ = has_schematic_ ? scene_->height : 500;
  zoom_ = 1.0;
  scroll_x_ = 0;
  scroll_y_ = 0;
  FitToWindow();
  InvalidateRect(window_, nullptr, TRUE);
}

void GateSchematicCanvas::SetMessage(std::wstring message) {
  scene_.reset();
  has_schematic_ = false;
  message_ = std::move(message);
  content_width_ = 800;
  content_height_ = 500;
  zoom_ = 1.0;
  scroll_x_ = 0;
  scroll_y_ = 0;
  UpdateScrollbars();
  InvalidateRect(window_, nullptr, TRUE);
}

void GateSchematicCanvas::SetNavigationCallbacks(ModuleOpenCallback open_module,
                                                 BackCallback go_back) {
  open_module_ = std::move(open_module);
  go_back_ = std::move(go_back);
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
    case WM_LBUTTONDBLCLK:
      SetFocus(window_);
      OpenModuleAt({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      return 0;
    case WM_MBUTTONDOWN:
      SetFocus(window_);
      BeginPan({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
      return 0;
    case WM_MBUTTONUP:
      EndPan();
      return 0;
    case WM_MOUSEMOVE:
      if (panning_) UpdatePan({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
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
      return 0;
    }
    case WM_KEYDOWN:
      if ((wparam == 'Z' && (GetKeyState(VK_CONTROL) & 0x8000) != 0) ||
          (wparam == 'F' && (GetKeyState(VK_SHIFT) & 0x8000) != 0)) {
        if (go_back_) go_back_();
      } else if (wparam == 'F' || wparam == '0') {
        FitToWindow();
      } else if (wparam == VK_ADD || wparam == VK_OEM_PLUS ||
                 wparam == VK_SUBTRACT || wparam == VK_OEM_MINUS) {
        RECT client{};
        GetClientRect(window_, &client);
        const bool increase = wparam == VK_ADD || wparam == VK_OEM_PLUS;
        ZoomAt(increase ? WHEEL_DELTA : -WHEEL_DELTA,
               {client.right / 2, client.bottom / 2});
      }
      return 0;
    case WM_NCDESTROY:
      window_ = nullptr;
      return 0;
    default:
      return DefWindowProcW(window_, message, wparam, lparam);
  }
}

void GateSchematicCanvas::OpenModuleAt(POINT cursor) {
  if (scene_ == nullptr || !open_module_) return;
  const double device_scale =
      static_cast<double>(GetDpiForWindow(window_)) / 96.0;
  const double scale = zoom_ * device_scale;
  if (scale <= 0.0) return;
  const int x = static_cast<int>((cursor.x + scroll_x_) / scale);
  const int y = static_cast<int>((cursor.y + scroll_y_) / scale);
  const auto found = std::find_if(
      scene_->nodes.begin(), scene_->nodes.end(), [x, y](const auto& node) {
        return node.kind == core::SchematicNodeKind::kModule &&
               x >= node.bounds.left && x <= node.bounds.right &&
               y >= node.bounds.top && y <= node.bounds.bottom;
      });
  if (found != scene_->nodes.end()) open_module_(found->type);
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
  if (!has_schematic_ || scene_ == nullptr) {
    SetTextColor(buffer, kSecondaryText);
    DrawCenteredText(buffer, message_, client,
                     DT_CENTER | DT_VCENTER | DT_WORDBREAK);
  } else {
    DrawGrid(buffer, window_, client, zoom_, scroll_x_, scroll_y_);
    SetGraphicsMode(buffer, GM_ADVANCED);
    const double device_scale =
        static_cast<double>(GetDpiForWindow(window_)) / 96.0;
    const FLOAT scale = static_cast<FLOAT>(zoom_ * device_scale);
    const SchematicViewport viewport = CalculateSchematicViewport(
        client.right, client.bottom, scroll_x_, scroll_y_, scale, 48);
    const SchematicDetail detail = viewport.detail;
    XFORM transform{scale,
                    0.0F,
                    0.0F,
                    scale,
                    static_cast<FLOAT>(-scroll_x_),
                    static_cast<FLOAT>(-scroll_y_)};
    SetWorldTransform(buffer, &transform);
    HFONT title_font = CreateSchematicFont(9, FW_SEMIBOLD);
    HFONT pin_font = CreateSchematicFont(6);
    HFONT label_font = CreateSchematicFont(8);
    HFONT instance_font = CreateSchematicFont(7);
    HGDIOBJ old_font = SelectObject(buffer, title_font);
    RECT title{24, 12, scene_->width - 24, 52};
    const std::wstring mode = scene_->mode == SchematicViewMode::kReadable
                                  ? L"Readable / inferred logic"
                                  : L"Gate / ABC cells";
    SetTextColor(buffer, kSchematicBlack);
    DrawCenteredText(
        buffer,
        L"Schematic — " + Utf8ToWide(scene_->top_module) + L" — " + mode, title,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    HPEN wire = CreatePen(PS_SOLID, 1, kSchematicBlack);
    HPEN bus = CreatePen(PS_SOLID, 2, kSchematicBlack);
    HBRUSH white = CreateSolidBrush(kCanvasBackground);
    HGDIOBJ old_pen = SelectObject(buffer, wire);
    HGDIOBJ old_brush = SelectObject(buffer, white);
    SelectObject(buffer, pin_font);
    struct BusLabelPlacement {
      OrthogonalPoint start;
      OrthogonalPoint end;
      std::size_t width = 1U;
      int length = 0;
    };
    std::map<std::string, BusLabelPlacement> bus_labels;
    for (const OrthogonalRoute& route : scene_->routing.routes) {
      const auto width = scene_->net_widths.find(route.net_id);
      const bool is_constant_route = route.net_id.starts_with("constant:");
      const bool is_bus =
          width != scene_->net_widths.end() && width->second > 1U;
      SelectObject(buffer, is_bus ? bus : wire);
      for (std::size_t index = 1; index < route.points.size(); ++index) {
        const OrthogonalPoint start = route.points[index - 1];
        const OrthogonalPoint end = route.points[index];
        if (!SchematicSegmentIsVisible(start, end, viewport.logical_bounds)) {
          continue;
        }
        MoveToEx(buffer, start.x, start.y, nullptr);
        LineTo(buffer, end.x, end.y);
        if (detail == SchematicDetail::kFull && is_bus && !is_constant_route) {
          const int length =
              std::abs(end.x - start.x) + std::abs(end.y - start.y);
          BusLabelPlacement& placement = bus_labels[route.net_id];
          if (length > placement.length) {
            placement = {start, end, width->second, length};
          }
        }
      }
    }
    SelectObject(buffer, bus);
    for (const auto& [net_id, placement] : bus_labels) {
      static_cast<void>(net_id);
      if (placement.length < 40) continue;
      const int x = (placement.start.x + placement.end.x) / 2;
      const int y = (placement.start.y + placement.end.y) / 2;
      MoveToEx(buffer, x - 3, y + 5, nullptr);
      LineTo(buffer, x + 3, y - 5);
      RECT width_label{x + 5, y - 14, x + 35, y + 3};
      DrawCenteredText(buffer, std::to_wstring(placement.width), width_label,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(buffer, wire);
    HBRUSH junction_brush = CreateSolidBrush(kJunctionBlue);
    SelectObject(buffer, junction_brush);
    for (const OrthogonalPoint junction : scene_->routing.junctions) {
      if (!SchematicPointIsVisible(junction, viewport.logical_bounds)) continue;
      Ellipse(buffer, junction.x - 3, junction.y - 3, junction.x + 4,
              junction.y + 4);
    }

    HBRUSH pin = CreateSolidBrush(kPinRed);
    SelectObject(buffer, white);
    SelectObject(buffer, wire);
    for (const SchematicSceneTerminal& terminal : scene_->terminals) {
      if (!SchematicPointIsVisible(terminal.point, viewport.logical_bounds)) {
        continue;
      }
      const bool constant = IsConstantTerminal(terminal);
      if (!constant) DrawPinSquare(buffer, terminal.point, pin);
      if (detail != SchematicDetail::kFull) continue;
      RECT label{terminal.point.x - 90, terminal.point.y - 10,
                 terminal.point.x - 8, terminal.point.y + 10};
      UINT format = DT_RIGHT | DT_VCENTER | DT_SINGLELINE;
      if (constant) {
        label = {terminal.point.x - 84, terminal.point.y - 10,
                 terminal.point.x - 6, terminal.point.y + 10};
        format = DT_RIGHT | DT_VCENTER | DT_SINGLELINE;
      } else if (terminal.direction != core::SchematicDirection::kInput) {
        label = {terminal.point.x + 8, terminal.point.y - 10,
                 terminal.point.x + 100, terminal.point.y + 10};
        format = DT_LEFT | DT_VCENTER | DT_SINGLELINE;
      }
      DrawCenteredText(buffer, Utf8ToWide(terminal.label), label, format);
    }
    for (const SchematicSceneNode& node : scene_->nodes) {
      if (!SchematicRectIsVisible(node.bounds, viewport.logical_bounds)) {
        continue;
      }
      DrawNode(buffer, node, detail, pin, pin_font, label_font, instance_font);
    }
    SelectObject(buffer, old_pen);
    SelectObject(buffer, old_brush);
    SelectObject(buffer, old_font);
    DeleteObject(pin);
    DeleteObject(junction_brush);
    DeleteObject(white);
    DeleteObject(bus);
    DeleteObject(wire);
    DeleteObject(instance_font);
    DeleteObject(label_font);
    DeleteObject(pin_font);
    DeleteObject(title_font);

    ModifyWorldTransform(buffer, nullptr, MWT_IDENTITY);
    SetGraphicsMode(buffer, GM_COMPATIBLE);
    const std::wstring navigation =
        std::to_wstring(static_cast<int>(zoom_ * 100.0 + 0.5)) +
        L"%   Double-click module: Open   Ctrl+Z / Shift+F: Back   Wheel: "
        L"Zoom   Middle-drag: Pan   F: Fit";
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
  const double scale =
      zoom_ * static_cast<double>(GetDpiForWindow(window_)) / 96.0;
  SCROLLINFO horizontal{
      sizeof(horizontal),
      SIF_ALL,
      0,
      std::max(0, static_cast<int>(content_width_ * scale) - 1),
      static_cast<UINT>(std::max(0L, client.right)),
      scroll_x_,
      0};
  scroll_x_ = SetScrollInfo(window_, SB_HORZ, &horizontal, TRUE);
  SCROLLINFO vertical{
      sizeof(vertical),
      SIF_ALL,
      0,
      std::max(0, static_cast<int>(content_height_ * scale) - 1),
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
  zoom_ = std::clamp(
      old_zoom * std::pow(1.15, static_cast<double>(wheel_delta) / WHEEL_DELTA),
      kMinimumZoom, kMaximumZoom);
  const double dpi = static_cast<double>(GetDpiForWindow(window_)) / 96.0;
  const double logical_x = (scroll_x_ + cursor.x) / (old_zoom * dpi);
  const double logical_y = (scroll_y_ + cursor.y) / (old_zoom * dpi);
  scroll_x_ = static_cast<int>(logical_x * zoom_ * dpi - cursor.x);
  scroll_y_ = static_cast<int>(logical_y * zoom_ * dpi - cursor.y);
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
  if (client.right <= 0 || client.bottom <= 0) return;
  const double dpi = static_cast<double>(GetDpiForWindow(window_)) / 96.0;
  zoom_ = CalculateSchematicFitZoom(content_width_, content_height_,
                                    client.right, client.bottom, dpi,
                                    Scale(window_, 28), Scale(window_, 48));
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
