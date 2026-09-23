// Aseprite
// Copyright (C) 2026 Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/commands/command.h"
#include "app/cmd/layer_from_background.h"
#include "app/cmd/set_cel_image.h"
#include "app/cmd/unlink_cel.h"
#include "app/context_access.h"
#include "app/doc.h"
#include "app/doc_api.h"
#include "app/i18n/strings.h"
#include "app/modules/gui.h"
#include "app/site.h"
#include "app/tx.h"
#include "doc/cel.h"
#include "doc/color.h"
#include "doc/image.h"
#include "doc/image_ref.h"
#include "doc/layer.h"
#include "doc/mask.h"
#include "doc/palette.h"
#include "doc/rgbmap.h"
#include "doc/sprite.h"
#include "gfx/point.h"
#include "ui/alert.h"

#include "isometric_extrude.xml.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace app {

namespace {

struct Vec3 {
  double x, y, z;
};

struct Vertex {
  double x, y, depth;
};

constexpr double pi = 3.14159265358979323846;

class Projection {
public:
  Projection(double xAngle, double yAngle, double zAngle, double centerX, double centerY,
             bool isometricPreset)
    : m_cx(centerX)
    , m_cy(centerY)
    , m_rx(xAngle * pi / 180.0)
    , m_ry(yAngle * pi / 180.0)
    , m_rz(zAngle * pi / 180.0)
    , m_pixelScale(isometricPreset ? std::sqrt(2.0) : 1.0)
  {
    const Vec3 axis = rotate({ 0, 0, 1 });
    const double projectedLength = std::hypot(axis.x, axis.y);
    m_depthScale = (projectedLength > 1e-6 ? 1.0 / (projectedLength * m_pixelScale) : 1.0);
  }

  Vec3 rotate(Vec3 p) const
  {
    double y = p.y * std::cos(m_rx) - p.z * std::sin(m_rx);
    double z = p.y * std::sin(m_rx) + p.z * std::cos(m_rx);
    p.y = y;
    p.z = z;
    double x = p.x * std::cos(m_ry) + p.z * std::sin(m_ry);
    z = -p.x * std::sin(m_ry) + p.z * std::cos(m_ry);
    p.x = x;
    p.z = z;
    x = p.x * std::cos(m_rz) - p.y * std::sin(m_rz);
    y = p.x * std::sin(m_rz) + p.y * std::cos(m_rz);
    return { x, y, p.z };
  }

  Vertex project(double x, double y, double z) const
  {
    const Vec3 p = rotate({ x - m_cx, y - m_cy, z * m_depthScale });
    return { p.x * m_pixelScale, p.y * m_pixelScale, p.z };
  }

  double light(Vec3 normal, int surface) const
  {
    if (surface == 1)
      return 1.0;
    const Vec3 n = rotate(normal);
    const double diffuse = std::max(0.0, n.x * -0.35 + n.y * -0.45 + n.z * 0.82);
    double value = 0.45 + 0.55 * diffuse;
    if (surface == 3)
      value = std::min(1.35, value + 0.35 * std::pow(diffuse, 12.0));
    return value;
  }

private:
  double m_cx, m_cy, m_rx, m_ry, m_rz, m_pixelScale, m_depthScale;
};

doc::color_t source_color(const doc::Image* image,
                          int x,
                          int y,
                          const doc::Palette* palette)
{
  const doc::color_t c = image->getPixel(x, y);
  switch (image->pixelFormat()) {
    case doc::IMAGE_RGB: return c;
    case doc::IMAGE_GRAYSCALE:
      return doc::rgba(doc::graya_getv(c), doc::graya_getv(c), doc::graya_getv(c),
                       doc::graya_geta(c));
    case doc::IMAGE_INDEXED:
      return (c == image->maskColor() ? 0 : palette->entry(c));
    default: return 0;
  }
}

doc::color_t shade(doc::color_t c, double factor)
{
  auto channel = [factor](int value) { return std::clamp(int(std::round(value * factor)), 0, 255); };
  return doc::rgba(channel(doc::rgba_getr(c)), channel(doc::rgba_getg(c)),
                   channel(doc::rgba_getb(c)), doc::rgba_geta(c));
}

doc::color_t target_color(doc::color_t c,
                          doc::PixelFormat format,
                          const doc::RgbMap* rgbmap)
{
  if (format == doc::IMAGE_GRAYSCALE)
    return doc::graya(doc::rgba_luma(c), doc::rgba_geta(c));
  if (format == doc::IMAGE_INDEXED)
    return rgbmap->mapColor(c);
  return c;
}

struct Raster {
  doc::Image* image;
  doc::PixelFormat format;
  const doc::RgbMap* rgbmap;
  int x, y;
  std::vector<double> zbuffer;
  std::vector<uint8_t> coverage;

  Raster(doc::Image* image, int x, int y, const doc::RgbMap* rgbmap)
    : image(image)
    , format(image->pixelFormat())
    , rgbmap(rgbmap)
    , x(x)
    , y(y)
    , zbuffer(size_t(image->width()) * image->height(), -std::numeric_limits<double>::infinity())
    , coverage(size_t(image->width()) * image->height(), 0)
  {
  }

  void pixel(int px, int py, double depth, doc::color_t color)
  {
    px -= x;
    py -= y;
    if (px < 0 || py < 0 || px >= image->width() || py >= image->height() ||
        doc::rgba_geta(color) == 0)
      return;
    const size_t i = size_t(py) * image->width() + px;
    if (depth < zbuffer[i])
      return;
    zbuffer[i] = depth;
    coverage[i] = 1;
    image->putPixel(px, py, target_color(color, format, rgbmap));
  }

  void triangle(Vertex a, Vertex b, Vertex c, doc::color_t color)
  {
    const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::abs(area) < 1e-9)
      return;
    const int left = std::max(x, int(std::floor(std::min({ a.x, b.x, c.x }))));
    const int top = std::max(y, int(std::floor(std::min({ a.y, b.y, c.y }))));
    const int right = std::min(x + image->width() - 1,
                               int(std::ceil(std::max({ a.x, b.x, c.x }))));
    const int bottom = std::min(y + image->height() - 1,
                                int(std::ceil(std::max({ a.y, b.y, c.y }))));
    for (int py = top; py <= bottom; ++py) {
      for (int px = left; px <= right; ++px) {
        const double sx = px + 0.5, sy = py + 0.5;
        const double u = ((b.x - sx) * (c.y - sy) - (b.y - sy) * (c.x - sx)) / area;
        const double v = ((c.x - sx) * (a.y - sy) - (c.y - sy) * (a.x - sx)) / area;
        const double w = 1.0 - u - v;
        if (u >= -1e-9 && v >= -1e-9 && w >= -1e-9)
          pixel(px, py, u * a.depth + v * b.depth + w * c.depth, color);
      }
    }
  }

  void quad(Vertex a, Vertex b, Vertex c, Vertex d, doc::color_t color)
  {
    triangle(a, b, c, color);
    triangle(a, c, d, color);
  }

  void line(Vertex a, Vertex b, doc::color_t color)
  {
    const int steps = std::max(1, int(std::ceil(std::max(std::abs(b.x - a.x),
                                                       std::abs(b.y - a.y)))));
    for (int i = 0; i <= steps; ++i) {
      const double t = double(i) / steps;
      pixel(int(std::floor(a.x + (b.x - a.x) * t)),
            int(std::floor(a.y + (b.y - a.y) * t)),
            a.depth + (b.depth - a.depth) * t,
            color);
    }
  }
};

class IsometricExtrudeWindow : public app::gen::IsometricExtrude {
public:
  IsometricExtrudeWindow()
  {
    position()->addItem(Strings::isometric_extrude_top());
    position()->addItem(Strings::isometric_extrude_left());
    position()->addItem(Strings::isometric_extrude_right());
    position()->addItem(Strings::isometric_extrude_bottom());
    position()->addItem(Strings::isometric_extrude_custom());
    surface()->addItem(Strings::isometric_extrude_wireframe());
    surface()->addItem(Strings::isometric_extrude_no_shading());
    surface()->addItem(Strings::isometric_extrude_diffuse());
    surface()->addItem(Strings::isometric_extrude_plastic());
    position()->setSelectedItemIndex(0);
    surface()->setSelectedItemIndex(2);
    depth()->setText("20");
    xRotation()->setText("50.768");
    yRotation()->setText("37.761");
    zRotation()->setText("-26.565");
    position()->Change.connect([this] {
      // Rotate the top view around the object's Y axis for the side views,
      // and around its X axis for the bottom view (Rz * Ry * Rx order).
      static const char* angles[4][3] = { { "50.768", "37.761", "-26.565" },
                                          { "45.000", "-30.000", "-90.000" },
                                          { "135.000", "30.000", "90.000" },
                                          { "-129.232", "37.761", "-26.565" } };
      const int selected = position()->getSelectedItemIndex();
      if (selected >= 0 && selected < 4) {
        m_pixelScale = true;
        m_updatingPreset = true;
        xRotation()->setText(angles[selected][0]);
        yRotation()->setText(angles[selected][1]);
        zRotation()->setText(angles[selected][2]);
        m_updatingPreset = false;
      }
      else if (selected == 4 && !m_editingPresetAngle)
        m_pixelScale = false;
    });
    auto custom = [this] {
      if (!m_updatingPreset && position()->getSelectedItemIndex() != 4) {
        m_editingPresetAngle = true;
        position()->setSelectedItemIndex(4);
        m_editingPresetAngle = false;
      }
    };
    xRotation()->Change.connect(custom);
    yRotation()->Change.connect(custom);
    zRotation()->Change.connect(custom);
  }

  bool pixelScale() const { return m_pixelScale; }

private:
  bool m_updatingPreset = false;
  bool m_editingPresetAngle = false;
  bool m_pixelScale = true;
};

} // namespace

class IsometricExtrudeCommand : public Command {
public:
  IsometricExtrudeCommand() : Command(CommandId::IsometricExtrude()) {}

protected:
  bool onEnabled(Context* context) override
  {
    if (!context->isUIAvailable() ||
        !context->checkFlags(ContextFlags::ActiveDocumentIsWritable |
                             ContextFlags::HasVisibleMask | ContextFlags::HasActiveCel |
                             ContextFlags::ActiveLayerIsEditable | ContextFlags::ActiveLayerIsImage))
      return false;
    const Site site = context->activeSite();
    return site.layer() && !site.layer()->isReference() &&
           site.cel() && site.cel()->image() &&
           site.cel()->image()->pixelFormat() != doc::IMAGE_TILEMAP &&
           site.cel()->image()->pixelFormat() != doc::IMAGE_BITMAP;
  }

  void onExecute(Context* context) override
  {
    IsometricExtrudeWindow window;
    window.openWindowInForeground();
    if (window.closer() != window.ok())
      return;

    const int depth = std::clamp(window.depth()->textInt(), 0, 1024);
    const int surface = window.surface()->getSelectedItemIndex();
    const Site site = context->activeSite();
    const doc::Mask* mask = site.document()->mask();
    const doc::Cel* cel = site.cel();
    if (!mask || mask->isEmpty() || !cel)
      return;
    const doc::Image* source = cel->image();
    const gfx::Rect bounds = mask->bounds();
    if (bounds.w > 2048 || bounds.h > 2048) {
      ui::Alert::show(Strings::isometric_extrude_too_large());
      return;
    }

    const Projection project(window.xRotation()->textDouble(), window.yRotation()->textDouble(),
                             window.zRotation()->textDouble(),
                             bounds.w / 2.0, bounds.h / 2.0,
                             window.pixelScale());
    double minX = std::numeric_limits<double>::infinity();
    double minY = minX, maxX = -minX, maxY = -minX;
    for (int zz : { 0, -depth }) {
      for (int yy : { 0, bounds.h }) {
        for (int xx : { 0, bounds.w }) {
          const Vertex p = project.project(xx, yy, zz);
          minX = std::min(minX, p.x);
          minY = std::min(minY, p.y);
          maxX = std::max(maxX, p.x);
          maxY = std::max(maxY, p.y);
        }
      }
    }
    const int offsetX = bounds.x + bounds.w / 2;
    const int offsetY = bounds.y + bounds.h / 2;
    const int effectX = int(std::floor(minX)) + offsetX - 1;
    const int effectY = int(std::floor(minY)) + offsetY - 1;
    const int effectW = int(std::ceil(maxX)) - int(std::floor(minX)) + 3;
    const int effectH = int(std::ceil(maxY)) - int(std::floor(minY)) + 3;
    const int left = std::min(cel->x(), effectX);
    const int top = std::min(cel->y(), effectY);
    const int right = std::max(cel->x() + source->width(), effectX + effectW);
    const int bottom = std::max(cel->y() + source->height(), effectY + effectH);
    if (right - left > 4096 || bottom - top > 4096 ||
        int64_t(right - left) * (bottom - top) > 16000000) {
      ui::Alert::show(Strings::isometric_extrude_too_large());
      return;
    }

    doc::ImageRef result(doc::Image::create(source->pixelFormat(), right - left, bottom - top));
    result->setMaskColor(source->maskColor());
    result->clear(source->maskColor());
    for (int y = 0; y < source->height(); ++y) {
      for (int x = 0; x < source->width(); ++x) {
        if (!mask->containsPoint(cel->x() + x, cel->y() + y))
          result->putPixel(cel->x() + x - left, cel->y() + y - top, source->getPixel(x, y));
      }
    }

    const doc::Palette* palette = site.palette();
    const doc::RgbMap* rgbmap = site.rgbMap();
    Raster raster(result.get(), left, top, rgbmap);
    auto vertex = [&](double x, double y, double z) {
      Vertex p = project.project(x, y, z);
      p.x += offsetX;
      p.y += offsetY;
      return p;
    };
    auto selected = [&](int x, int y) {
      const int sx = bounds.x + x - cel->x();
      const int sy = bounds.y + y - cel->y();
      return x >= 0 && y >= 0 && x < bounds.w && y < bounds.h &&
             sx >= 0 && sy >= 0 && sx < source->width() && sy < source->height() &&
             mask->containsPoint(bounds.x + x, bounds.y + y) &&
             doc::rgba_geta(source_color(source, sx, sy, palette)) > 0;
    };

    for (int y = 0; y < bounds.h; ++y) {
      for (int x = 0; x < bounds.w; ++x) {
        if (!selected(x, y))
          continue;
        const doc::color_t color = source_color(source, bounds.x + x - cel->x(),
                                                bounds.y + y - cel->y(), palette);
        const Vertex front[4] = { vertex(x, y, 0), vertex(x + 1, y, 0),
                                  vertex(x + 1, y + 1, 0), vertex(x, y + 1, 0) };
        const Vertex back[4] = { vertex(x, y, -depth), vertex(x + 1, y, -depth),
                                 vertex(x + 1, y + 1, -depth), vertex(x, y + 1, -depth) };
        if (surface != 0) {
          raster.quad(back[0], back[1], back[2], back[3],
                      shade(color, project.light({ 0, 0, -1 }, surface)));
          raster.quad(front[0], front[1], front[2], front[3],
                      shade(color, project.light({ 0, 0, 1 }, surface)));
        }
        const bool edge[4] = { !selected(x, y - 1), !selected(x + 1, y),
                               !selected(x, y + 1), !selected(x - 1, y) };
        const Vec3 normal[4] = { { 0, -1, 0 }, { 1, 0, 0 },
                                 { 0, 1, 0 }, { -1, 0, 0 } };
        for (int e = 0; e < 4; ++e) {
          if (!edge[e])
            continue;
          const int next = (e + 1) % 4;
          if (surface == 0) {
            raster.line(front[e], front[next], color);
            raster.line(back[e], back[next], color);
            raster.line(front[e], back[e], color);
            raster.line(front[next], back[next], color);
          }
          else {
            raster.quad(front[e], front[next], back[next], back[e],
                        shade(color, project.light(normal[e], surface)));
          }
        }
      }
    }

    if (std::none_of(raster.coverage.begin(), raster.coverage.end(),
                     [](uint8_t value) { return value != 0; }))
      return;

    doc::Mask newMask;
    newMask.reserve(gfx::Rect(effectX, effectY, effectW, effectH));
    for (int y = 0; y < effectH; ++y) {
      for (int x = 0; x < effectW; ++x) {
        if (raster.coverage[size_t(y + effectY - top) * result->width() + x + effectX - left])
          newMask.bitmap()->putPixel(x, y, 1);
      }
    }
    newMask.shrink();

    ContextWriter writer(context);
    Tx tx(writer, friendlyName());
    DocApi api = writer.document()->getApi(tx);
    if (writer.layer()->isBackground())
      tx(new cmd::LayerFromBackground(writer.layer()));
    if (writer.cel()->links())
      tx(new cmd::UnlinkCel(writer.cel()));
    tx(new cmd::SetCelImage(writer.cel(), result));
    api.setCelPosition(writer.sprite(), writer.cel(), left, top);
    api.copyToCurrentMask(&newMask);
    tx.commit();
    update_screen_for_document(writer.document());
  }
};

Command* CommandFactory::createIsometricExtrudeCommand()
{
  return new IsometricExtrudeCommand;
}

} // namespace app
