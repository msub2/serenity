/*
 * Copyright (c) 2022-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/URL.h>
#include <Ladybird/Types.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibGfx/StandardCursor.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ActivateTab.h>
#include <LibWebView/ViewImplementation.h>
#include <QAbstractScrollArea>
#include <QUrl>

class QTextEdit;
class QLineEdit;

namespace WebView {
class WebContentClient;
}

using WebView::WebContentClient;

namespace Ladybird {

class Tab;

class WebContentView final
    : public QAbstractScrollArea
    , public WebView::ViewImplementation {
    Q_OBJECT
public:
    WebContentView(WebContentOptions const&, StringView webdriver_content_ipc_path);
    virtual ~WebContentView() override;

    Function<String(const AK::URL&, Web::HTML::ActivateTab)> on_tab_open_request;

    virtual void paintEvent(QPaintEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual void wheelEvent(QWheelEvent*) override;
    virtual void mouseDoubleClickEvent(QMouseEvent*) override;
    virtual void dragEnterEvent(QDragEnterEvent*) override;
    virtual void dropEvent(QDropEvent*) override;
    virtual void keyPressEvent(QKeyEvent* event) override;
    virtual void keyReleaseEvent(QKeyEvent* event) override;
    virtual void showEvent(QShowEvent*) override;
    virtual void hideEvent(QHideEvent*) override;
    virtual void focusInEvent(QFocusEvent*) override;
    virtual void focusOutEvent(QFocusEvent*) override;
    virtual bool event(QEvent*) override;

    ErrorOr<String> dump_layout_tree();

    void set_viewport_rect(Gfx::IntRect);
    void set_window_size(Gfx::IntSize);
    void set_window_position(Gfx::IntPoint);
    void set_device_pixel_ratio(double);

    enum class PaletteMode {
        Default,
        Dark,
    };
    void update_palette(PaletteMode = PaletteMode::Default);

signals:
    void urls_dropped(QList<QUrl> const&);

private:
    // ^WebView::ViewImplementation
    virtual void create_client() override;
    virtual void update_zoom() override;
    virtual Web::DevicePixelRect viewport_rect() const override;
    virtual Gfx::IntPoint to_content_position(Gfx::IntPoint widget_position) const override;
    virtual Gfx::IntPoint to_widget_position(Gfx::IntPoint content_position) const override;

    void update_viewport_rect();
    void update_cursor(Gfx::StandardCursor cursor);

    bool m_should_show_line_box_borders { false };

    Gfx::IntRect m_viewport_rect;

    WebContentOptions m_web_content_options;
    StringView m_webdriver_content_ipc_path;
};

}
