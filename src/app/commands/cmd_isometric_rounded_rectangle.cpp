// Aseprite
// Copyright (C) 2026 Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/commands/command.h"
#include "app/cmd/set_cel_image.h"
#include "app/cmd/unlink_cel.h"
#include "app/color_utils.h"
#include "app/context_access.h"
#include "app/doc.h"
#include "app/doc_api.h"
#include "app/i18n/strings.h"
#include "app/modules/gui.h"
#include "app/pref/preferences.h"
#include "app/site.h"
#include "app/tx.h"
#include "doc/cel.h"
#include "doc/image.h"
#include "doc/image_ref.h"
#include "doc/layer.h"
#include "doc/mask.h"
#include "doc/sprite.h"
#include "gfx/rect.h"
#include "ui/alert.h"

#include "isometric_rounded_rectangle.xml.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace app {

namespace {

class IsometricRoundedRectangleCommand : public Command {
public:
  IsometricRoundedRectangleCommand() : Command(CommandId::IsometricRoundedRectangle()) {}

protected:
  bool onEnabled(Context* context) override
  {
    if (!context->isUIAvailable() ||
        !context->checkFlags(ContextFlags::ActiveDocumentIsWritable |
                             ContextFlags::ActiveLayerIsVisible |
                             ContextFlags::ActiveLayerIsEditable |
                             ContextFlags::ActiveLayerIsImage))
      return false;
    const Site site = context->activeSite();
    return site.sprite() && site.sprite()->pixelFormat() != doc::IMAGE_BITMAP &&
           site.layer() && !site.layer()->isReference() && !site.layer()->isTilemap();
  }

  void onExecute(Context* context) override
  {
    const Site site = context->activeSite();
    const doc::Sprite* sprite = site.sprite();
    if (!sprite)
      return;

    app::gen::IsometricRoundedRectangle window;
    window.length()->setText("16");
    window.width()->setText("16");
    window.radius()->setText("4");
    const doc::Mask* mask = site.document()->mask();
    const bool selected = site.document()->isMaskVisible() && mask && !mask->isEmpty();
    const gfx::Rect bounds = selected ? mask->bounds() : sprite->bounds();
    window.centerX()->setTextf("%d", bounds.x + bounds.w / 2);
    window.centerY()->setTextf("%d", bounds.y + bounds.h / 2);
    window.openWindowInForeground();
    if (window.closer() != window.ok())
      return;

    const int length = std::clamp(window.length()->textInt(), 1, 2048);
    const int width = std::clamp(window.width()->textInt(), 1, 2048);
    const int radius = std::clamp(window.radius()->textInt(), 0, std::min(length, width) / 2);
    const int centerX = std::clamp(window.centerX()->textInt(), 0, sprite->width());
    const int centerY = std::clamp(window.centerY()->textInt(), 0, sprite->height());
    const double halfSpan = (length + width) / 2.0;
    const int shapeLeft = std::max(0, int(std::floor(centerX - halfSpan)));
    const int shapeTop = std::max(0, int(std::floor(centerY - halfSpan / 2.0)));
    const int shapeRight = std::min(sprite->width(), int(std::ceil(centerX + halfSpan)));
    const int shapeBottom = std::min(sprite->height(), int(std::ceil(centerY + halfSpan / 2.0)));
    if (shapeRight <= shapeLeft || shapeBottom <= shapeTop)
      return;

    ContextWriter writer(context);
    doc::LayerImage* layer = static_cast<doc::LayerImage*>(writer.layer());
    doc::Cel* cel = layer->cel(writer.frame());
    const doc::Image* source = cel ? cel->image() : nullptr;
    const int left = source ? std::min(shapeLeft, cel->x()) : shapeLeft;
    const int top = source ? std::min(shapeTop, cel->y()) : shapeTop;
    const int right = source ? std::max(shapeRight, cel->x() + source->width()) : shapeRight;
    const int bottom = source ? std::max(shapeBottom, cel->y() + source->height()) : shapeBottom;
    if (right - left > 8192 || bottom - top > 8192 ||
        int64_t(right - left) * (bottom - top) > 16000000) {
      ui::Alert::show(Strings::isometric_rounded_rectangle_too_large());
      return;
    }

    doc::ImageRef result(doc::Image::create(sprite->pixelFormat(), right - left, bottom - top));
    result->setMaskColor(source ? source->maskColor() : sprite->transparentColor());
    result->clear(result->maskColor());
    if (source) {
      for (int y = 0; y < source->height(); ++y) {
        for (int x = 0; x < source->width(); ++x)
          result->putPixel(cel->x() + x - left, cel->y() + y - top, source->getPixel(x, y));
      }
    }

    const doc::color_t color =
      color_utils::color_for_layer(Preferences::instance().colorBar.fgColor(), layer);
    bool drawn = false;
    for (int y = shapeTop; y < shapeBottom; ++y) {
      for (int x = shapeLeft; x < shapeRight; ++x) {
        if (selected && !mask->containsPoint(x, y))
          continue;
        const double px = x + 0.5 - centerX;
        const double py = y + 0.5 - centerY;
        const double u = py + px / 2.0;
        const double v = py - px / 2.0;
        const double du = std::max(0.0, std::abs(u) - (length / 2.0 - radius));
        const double dv = std::max(0.0, std::abs(v) - (width / 2.0 - radius));
        if (du * du + dv * dv > double(radius) * radius)
          continue;
        result->putPixel(x - left, y - top, color);
        drawn = true;
      }
    }
    if (!drawn)
      return;

    Tx tx(writer, friendlyName());
    DocApi api = writer.document()->getApi(tx);
    if (cel) {
      if (cel->links())
        tx(new cmd::UnlinkCel(cel));
      tx(new cmd::SetCelImage(cel, result));
      api.setCelPosition(writer.sprite(), cel, left, top);
    }
    else {
      cel = api.addCel(layer, writer.frame(), result);
      api.setCelPosition(writer.sprite(), cel, left, top);
    }
    tx.commit();
    update_screen_for_document(writer.document());
  }
};

} // namespace

Command* CommandFactory::createIsometricRoundedRectangleCommand()
{
  return new IsometricRoundedRectangleCommand;
}

} // namespace app
