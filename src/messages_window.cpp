// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include "messages_window.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <ctime>

#include "base/i18n.h"
#include "ui/button.h"
#include "ui/icon.h"
#include "ui/image_view.h"
#include "ui/label.h"
#include "ui/list_view.h"
#include "ui/keys.h"
#include "ui/scroll_view.h"
#include "ui/text_field.h"
#include "ui/theme.h"
#include "ui/window_header.h"

using cutefish::i18n::tr;

namespace cutefish::messages {

namespace {

std::string timeOf(const std::string &timestamp)
{
    if (timestamp.size() < 16)
        return {};
    // "2026-11-22T10:32:00…" or a display timestamp as the daemon gives it.
    const size_t space = timestamp.find(' ');
    if (space != std::string::npos)
        return timestamp.substr(space + 1, 5);
    const size_t t = timestamp.find('T');
    if (t != std::string::npos)
        return timestamp.substr(t + 1, 5);
    return {};
}

std::string initialOf(const std::string &name)
{
    for (const char c : name) {
        if (std::isalpha(static_cast<unsigned char>(c)))
            return std::string(1, char(std::toupper(static_cast<unsigned char>(c))));
    }
    return "#";
}

// The daemon's timestamps are text; the phone's are epoch milliseconds.
std::string timestampOf(int64_t milliseconds)
{
    const std::time_t seconds = std::time_t(milliseconds / 1000);
    std::tm local {};
    localtime_r(&seconds, &local);
    char text[32] = {};
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &local);
    return text;
}

int64_t nowMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
}

bool looksLikeUrl(const std::string &text, size_t at, size_t &length)
{
    static const std::array<const char *, 3> schemes = {"https://", "http://", "www."};
    for (const char *scheme : schemes) {
        const size_t size = std::strlen(scheme);
        if (text.compare(at, size, scheme) != 0)
            continue;
        size_t end = at + size;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])))
            ++end;
        // Trailing punctuation belongs to the sentence, not the address.
        while (end > at && std::strchr(".,;:!?)]}'\"", text[end - 1]))
            --end;
        if (end - at <= size)
            return false;
        length = end - at;
        return true;
    }
    return false;
}

std::vector<ui::Label::Span> spansFor(const std::string &text, bool &hasLink)
{
    std::vector<ui::Label::Span> spans;
    size_t start = 0;
    for (size_t at = 0; at < text.size();) {
        size_t length = 0;
        if (!looksLikeUrl(text, at, length)) {
            ++at;
            continue;
        }
        if (at > start) {
            ui::Label::Span plain;
            plain.text = text.substr(start, at - start);
            spans.push_back(std::move(plain));
        }
        ui::Label::Span link;
        link.text = text.substr(at, length);
        link.link = link.text.rfind("www.", 0) == 0 ? "https://" + link.text : link.text;
        link.underline = true;
        spans.push_back(std::move(link));
        hasLink = true;
        at += length;
        start = at;
    }
    if (!hasLink)
        return {};
    if (start < text.size()) {
        ui::Label::Span plain;
        plain.text = text.substr(start);
        spans.push_back(std::move(plain));
    }
    return spans;
}

void openLink(const std::string &url)
{
    const pid_t child = ::fork();
    if (child == 0) {
        ::setsid();
        ::execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char *>(nullptr));
        ::_exit(1);
    }
    if (child > 0)
        ::waitpid(child, nullptr, 0);
}

int64_t secondsOf(const std::string &timestamp)
{
    std::tm parsed {};
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (std::sscanf(timestamp.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute,
                    &second) != 6)
        return 0;
    parsed.tm_year = year - 1900;
    parsed.tm_mon = month - 1;
    parsed.tm_mday = day;
    parsed.tm_hour = hour;
    parsed.tm_min = minute;
    parsed.tm_sec = second;
    return int64_t(timegm(&parsed));
}

Message messageOf(const phone::SmsMessage &sms)
{
    Message message;
    message.id = sms.id;
    message.handle = sms.address;
    message.body = sms.body;
    // The original when it has been fetched, unless it is something only the
    // preview can show.
    const bool playable = sms.fullPath.ends_with(".mp4") || sms.fullPath.ends_with(".mp3")
            || sms.fullPath.ends_with(".bin");
    if (!sms.fullPath.empty() && !playable) {
        message.attachmentPath = sms.fullPath;
    } else {
        for (const phone::SmsAttachment &attachment : sms.attachments) {
            if (!attachment.thumbnailPath.empty()) {
                message.attachmentPath = attachment.thumbnailPath;
                break;
            }
        }
    }
    message.timestamp = timestampOf(sms.date);
    message.outgoing = sms.type == 2;
    message.read = sms.read;
    return message;
}

// "+14803460971" reads as a person, not as a number.
std::string prettyAddress(const std::string &address)
{
    std::string digits;
    for (const char c : address) {
        if (std::isdigit(static_cast<unsigned char>(c)))
            digits.push_back(c);
    }
    if (address.size() > 1 && address[0] == '+' && digits.size() == 11 && digits[0] == '1')
        return "+1 (" + digits.substr(1, 3) + ") " + digits.substr(4, 3) + "-" + digits.substr(7);
    if (digits.size() == 10 && digits == address)
        return "(" + digits.substr(0, 3) + ") " + digits.substr(3, 3) + "-" + digits.substr(6);
    return address;
}

} // namespace

class SidebarRow : public ui::View
{
public:
    SidebarRow()
        : m_avatar(addChild<ui::Label>())
        , m_name(addChild<ui::Label>())
        , m_snippet(addChild<ui::Label>())
        , m_time(addChild<ui::Label>())
    {
        m_avatar->setTextAlignment(ui::Label::TextAlignment::Center);
        m_avatar->setVerticallyCentered(true);
        m_name->setElide(ui::Label::Elide::Right);
        m_name->setFadesWhenDisabled(false);
        m_snippet->setElide(ui::Label::Elide::Right);
        m_snippet->setFadesWhenDisabled(false);
        m_time->setFadesWhenDisabled(false);
    }

    void bind(const Thread &thread, bool selected)
    {
        const ui::Theme &theme = ui::Theme::current();
        m_selected = selected;
        m_avatar->setText(initialOf(thread.name));
        m_avatar->setFontSize(theme.fontSize() + 1);
        m_avatar->setFontFamily(theme.fontFamily);
        m_name->setText(thread.name);
        m_name->setFontSize(theme.fontSize());
        m_name->setFontFamily(theme.fontFamily);
        m_name->setColor(ui::ColorId::Text);
        m_snippet->setText(thread.snippet);
        m_snippet->setFontSize(theme.smallFontSize());
        m_snippet->setFontFamily(theme.fontFamily);
        m_snippet->setColor(ui::ColorId::DisabledText);
        m_unread = thread.unread > 0;
        m_time->setText(timeOf(thread.lastTimestamp));
        m_time->setFontSize(theme.smallFontSize());
        m_time->setFontFamily(theme.fontFamily);
        m_time->setColor(ui::ColorId::DisabledText);
        requestLayout();
        requestPaint();
    }

    bool acceptsPointer() const override { return true; }
    std::function<void()> onClicked;

protected:
    void pointerPress(const ui::PointerEvent &event) override
    {
        if (event.button == ui::MouseButton::Left && onClicked)
            onClicked();
    }

    void layout() override
    {
        const double height = geometry().height;
        m_avatar->setGeometry({16, (height - 34) / 2, 34, 34});
        const double textLeft = 60;
        const double width = std::max(0.0, geometry().width - textLeft - 12);
        m_time->setGeometry({geometry().width - 54, 12, 44, 16});
        m_name->setGeometry({textLeft, 10, width - 46, 20});
        m_snippet->setGeometry({textLeft, 31, width, 18});
    }

    void paint(ui::Painter &painter) override
    {
        const ui::Theme &theme = ui::Theme::current();
        painter.fillEllipse({16, (geometry().height - 34) / 2, 34, 34},
                            m_selected ? theme.highlight.withAlpha(0.22)
                                       : (theme.darkMode ? theme.secondBackground
                                                         : ui::Color::rgb(0xE4E4E8)));
        if (m_unread && !m_selected)
            painter.fillEllipse({4, (geometry().height - 8) / 2, 8, 8}, theme.highlight);
        if (m_selected)
            painter.drawRoundedRect({6, 3, geometry().width - 12, geometry().height - 6}, {8, 0, ui::themeCorners()},
                                    ui::Theme::current().highlight.withAlpha(0.14));
    }

private:
    ui::Label *m_avatar;
    ui::Label *m_name;
    ui::Label *m_snippet;
    ui::Label *m_time;
    bool m_selected = false;
    bool m_unread = false;
};

class BubbleView : public ui::View
{
public:
    BubbleView()
        : m_image(addChild<ui::ImageView>())
        , m_author(addChild<ui::Label>())
        , m_body(addChild<ui::Label>())
        , m_time(addChild<ui::Label>())
    {
        m_image->setCornerRadius(12);
        m_image->setFillMode(ui::ImageView::FillMode::PreserveAspectFit);
        m_image->setVisible(false);
        // The preview is decoded after the row is measured; once it knows its
        // shape, the bubble takes it.
        m_image->onStatusChanged = [this](ui::ImageView::Status status) {
            if (status != ui::ImageView::Status::Ready)
                return;
            const ui::SizeF size = m_image->sizeHint();
            if (size.width <= 0 || size.height <= 0)
                return;
            const double aspect = size.width / size.height;
            if (std::abs(aspect - m_aspect) < 0.01)
                return;
            m_aspect = aspect;
            requestLayout();
        };
        m_author->setFadesWhenDisabled(false);
        m_body->setFadesWhenDisabled(false);
        m_body->setWrap(true);
        m_body->setVerticallyCentered(true);
        m_time->setFadesWhenDisabled(false);
    }

    void bind(const Message &message, bool group, bool showAuthor, bool showTime)
    {
        const ui::Theme &theme = ui::Theme::current();
        m_messageId = message.id;
        m_outgoing = message.outgoing;
        m_hasImage = !message.attachmentPath.empty();
        m_hasText = !message.body.empty();
        m_image->setVisible(m_hasImage);
        if (m_hasImage)
            m_image->setSource(message.attachmentPath);
        m_body->setVisible(m_hasText);
        m_showTime = showTime;
        m_time->setVisible(showTime);
        m_showAuthor = group && !message.outgoing && showAuthor && !message.sender.empty();
        m_author->setVisible(m_showAuthor);
        m_author->setText(m_showAuthor ? message.sender : std::string());
        m_author->setFontSize(theme.smallFontSize());
        m_author->setFontFamily(theme.fontFamily);
        m_author->setColor(ui::ColorId::DisabledText);
        bool hasLink = false;
        const std::vector<ui::Label::Span> spans = spansFor(message.body, hasLink);
        if (hasLink)
            m_body->setSpans(spans);
        else
            m_body->setText(message.body);
        m_hasLink = hasLink;
        m_body->setFontSize(theme.fontSize());
        m_body->setFontFamily(theme.fontFamily);
        m_body->setColor(message.outgoing ? ui::ColorId::HighlightedText : ui::ColorId::Text);
        m_time->setText(timeOf(message.timestamp));
        m_time->setFontSize(theme.smallFontSize());
        m_time->setFontFamily(theme.fontFamily);
        m_time->setColor(ui::ColorId::DisabledText);
        m_time->setTextAlignment(message.outgoing ? ui::Label::TextAlignment::Right
                                                  : ui::Label::TextAlignment::Left);
    }

    // The height this bubble needs at a row of this width, with the text wrapped.
    double measure(double rowWidth)
    {
        const double maximum = std::min(rowWidth * 0.68, 460.0);
        // As wide as the text wants, up to the maximum, and never a sliver.
        const double wanted = m_hasText ? m_body->sizeHint().width + 32.0 : 0.0;
        m_bubbleWidth = std::clamp(std::max(wanted, m_hasImage ? 220.0 : 0.0), 64.0, maximum);
        const double textWidth = std::max(40.0, m_bubbleWidth - 32);
        m_bodyHeight = m_hasText
                ? std::max(22.0, std::ceil(m_body->heightForWidth(textWidth)) + 2.0)
                : 0.0;
        m_imageHeight = m_hasImage ? std::clamp((m_bubbleWidth - 20) / m_aspect, 90.0, 340.0)
                                   : 0.0;
        m_authorHeight = m_showAuthor ? 17.0 : 0.0;
        double inner = 0;
        if (m_hasImage)
            inner += m_imageHeight;
        if (m_hasText) {
            if (inner > 0)
                inner += 8;
            inner += m_bodyHeight;
        }
        m_bubbleHeight = 10 + m_authorHeight + inner + 10;
        return m_bubbleHeight + (m_showTime ? 17.0 : 0.0);
    }

    void setAttachment(const std::string &path)
    {
        m_hasImage = true;
        m_image->setSource(path);
        m_image->setVisible(true);
        requestLayout();
        requestPaint();
    }

    bool acceptsPointer() const override { return m_hasImage || m_hasLink; }
    std::function<void(int64_t, const std::string &)> onImage;

protected:
    void pointerPress(const ui::PointerEvent &event) override
    {
        if (event.button != ui::MouseButton::Left)
            return;
        if (m_hasLink) {
            const ui::RectF body = m_body->geometry();
            const std::string link = m_body->linkAt(
                    {event.position.x - body.x, event.position.y - body.y});
            if (!link.empty()) {
                openLink(link);
                return;
            }
        }
        if (m_hasImage && onImage)
            onImage(m_messageId, m_image->source());
    }

    void layout() override
    {
        m_left = m_outgoing ? geometry().width - m_bubbleWidth - 18 : 18;
        m_author->setGeometry({m_left + 16, 10, m_bubbleWidth - 32, m_authorHeight});
        double y = 10 + m_authorHeight;
        if (m_hasImage) {
            m_image->setGeometry({m_left + 10, y, m_bubbleWidth - 20, m_imageHeight});
            y += m_imageHeight;
            if (m_hasText)
                y += 8;
        }
        if (m_hasText)
            m_body->setGeometry({m_left + 16, y, m_bubbleWidth - 32, m_bodyHeight});
        m_time->setGeometry({m_left + 16, m_bubbleHeight + 3, m_bubbleWidth - 32,
                             m_showTime ? 13.0 : 0.0});
    }

    void paint(ui::Painter &painter) override
    {
        const ui::Theme &theme = ui::Theme::current();
        const ui::Color bubble = m_outgoing
                ? theme.highlight.withAlpha(0.92)
                : (theme.darkMode ? ui::Color::rgb(0x3A3A3C) : ui::Color::rgb(0xE9E9EB));
        painter.drawRoundedRect({m_left, 0, m_bubbleWidth, m_bubbleHeight},
                                {17, 0, ui::themeCorners()}, bubble);
    }

private:
    ui::ImageView *m_image;
    ui::Label *m_author;
    ui::Label *m_body;
    ui::Label *m_time;
    bool m_hasImage = false;
    bool m_hasText = true;
    bool m_hasLink = false;
    int64_t m_messageId = 0;
    bool m_outgoing = false;
    bool m_showAuthor = false;
    bool m_showTime = true;
    double m_left = 0;
    double m_bubbleWidth = 0;
    double m_bubbleHeight = 0;
    double m_bodyHeight = 21;
    double m_imageHeight = 0;
    double m_aspect = 1.4;
    double m_authorHeight = 0;
};

// The conversation itself: bubbles laid out at their measured heights, since
// a wrapped message has no fixed height the way a list row does.
class ConversationView : public ui::View
{
public:
    ConversationView(std::vector<Message> messages, bool group,
                     std::function<void(int64_t, const std::string &)> openImage)
        : m_messages(std::move(messages))
        , m_group(group)
    {
        for (size_t i = 0; i < m_messages.size(); ++i) {
            auto *bubble = addChild<BubbleView>();
            bubble->onImage = openImage;
            const bool sameSender = i > 0 && m_messages[i - 1].sender == m_messages[i].sender;
            const bool showTime = i == 0 || !sameSender || breaksGroup(i)
                    || secondsOf(m_messages[i].timestamp)
                            - secondsOf(m_messages[i - 1].timestamp) >= 600;
            bubble->bind(m_messages[i], m_group, !sameSender, showTime);
            m_bubbles.push_back(bubble);
        }
    }

    double heightForWidth(double width) const override
    {
        const double rowWidth = std::max(0.0, width - 16);
        double total = 8;
        for (size_t i = 0; i < m_bubbles.size(); ++i) {
            if (i > 0 && breaksGroup(i))
                total += 10;
            total += m_bubbles[i]->measure(rowWidth) + 4;
        }
        return total + 8;
    }

protected:
    void layout() override
    {
        const double rowWidth = std::max(0.0, geometry().width - 16);
        double y = 8;
        for (size_t i = 0; i < m_bubbles.size(); ++i) {
            if (i > 0 && breaksGroup(i))
                y += 10;
            const double height = m_bubbles[i]->measure(rowWidth);
            m_bubbles[i]->setGeometry({8, y, rowWidth, height});
            y += height + 4;
        }
    }

private:
public:
    void updateAttachment(int64_t id, const std::string &path)
    {
        for (size_t i = 0; i < m_messages.size(); ++i) {
            if (m_messages[i].id != id)
                continue;
            m_messages[i].attachmentPath = path;
            m_bubbles[i]->setAttachment(path);
            requestLayout();
            return;
        }
    }

private:
    bool breaksGroup(size_t i) const
    {
        return m_messages[i].outgoing != m_messages[i - 1].outgoing
                || m_messages[i].sender != m_messages[i - 1].sender;
    }

    std::vector<Message> m_messages;
    std::vector<BubbleView *> m_bubbles;
    bool m_group = false;
};

class SendButton : public ui::View
{
public:
    SendButton()
        : m_glyph(addChild<ui::Label>("↑"))
    {
        m_glyph->setTextAlignment(ui::Label::TextAlignment::Center);
        m_glyph->setVerticallyCentered(true);
        m_glyph->setFadesWhenDisabled(false);
        m_glyph->setFontSize(ui::Theme::current().fontSize() + 7);
        m_glyph->setColor(ui::ColorId::HighlightedText);
    }

    bool acceptsPointer() const override { return true; }
    std::function<void()> onClicked;

protected:
    void layout() override
    {
        m_glyph->setGeometry({0, 1, geometry().width, geometry().height});
    }

    void paint(ui::Painter &painter) override
    {
        const ui::Theme &theme = ui::Theme::current();
        painter.fillEllipse({0, 0, geometry().width, geometry().height},
                            isEnabled() ? theme.highlight : theme.secondBackground);
        m_glyph->setColor(isEnabled() ? ui::ColorId::HighlightedText : ui::ColorId::DisabledText);
    }

    void pointerPress(const ui::PointerEvent &event) override
    {
        if (event.button == ui::MouseButton::Left && isEnabled() && onClicked)
            onClicked();
    }

private:
    ui::Label *m_glyph;
};

// A picture at the window's size over a dimmed scrim; a click or Escape closes it.
class ImageViewer : public ui::View
{
public:
    ImageViewer()
        : m_image(addChild<ui::ImageView>())
    {
        m_image->setFillMode(ui::ImageView::FillMode::PreserveAspectFit);
        m_image->setCornerRadius(12);
        setVisible(false);
    }

    void show(int64_t messageId, const std::string &path)
    {
        m_messageId = messageId;
        m_image->setSource(path);
        setVisible(true);
        setFocus();
        requestLayout();
        requestPaint();
    }

    // The original arrived for what is on screen: show it instead.
    void updateIfShowing(int64_t messageId, const std::string &path)
    {
        if (isVisible() && messageId == m_messageId) {
            m_image->setSource(path);
            requestPaint();
        }
    }

    bool acceptsPointer() const override { return true; }
    bool acceptsFocus() const override { return true; }

protected:
    void layout() override
    {
        m_image->setGeometry({48, 40, std::max(0.0, geometry().width - 96),
                              std::max(0.0, geometry().height - 80)});
    }

    void paint(ui::Painter &painter) override
    {
        painter.fillRect({0, 0, geometry().width, geometry().height},
                         ui::Color::rgb(0x000000).withAlpha(0.72));
    }

    void pointerPress(const ui::PointerEvent &event) override
    {
        if (event.button == ui::MouseButton::Left)
            setVisible(false);
    }

    bool keyPress(const ui::KeyEvent &event) override
    {
        if (event.key == ui::key::Escape) {
            setVisible(false);
            return true;
        }
        return false;
    }

private:
    ui::ImageView *m_image;
    int64_t m_messageId = 0;
};

class MessagesWindow::RootView : public ui::View
{
public:
    explicit RootView(MessagesWindow &window)
        : m_window(window)
        , m_header(addChild<ui::WindowHeader>(44))
        , m_sidebarTitle(addChild<ui::Label>(tr("Messages")))
        , m_sidebar(addChild<ui::ListView>())
        , m_title(addChild<ui::Label>())
        , m_subtitle(addChild<ui::Label>())
        , m_messages(addChild<ui::ScrollView>())
        , m_empty(addChild<ui::Label>())
        , m_composer(addChild<ui::TextField>())
        , m_send(addChild<SendButton>())
        , m_status(addChild<ui::Label>())
        , m_viewer(addChild<ImageViewer>())
    {
        m_header->setContentUnderButtons(true);
        m_header->setButtonMargin(14);
        setClipsChildren(true);
        setChildClipRadius(24);
        m_sidebarTitle->setFontSize(ui::Theme::current().fontSize() + 3);
        m_sidebarTitle->setFadesWhenDisabled(false);
        m_sidebar->setRowHeight(56);
        m_sidebar->setClipsChildren(true);
        m_sidebar->setVerticalBarPolicy(ui::ScrollView::BarPolicy::AsNeeded);
        m_sidebar->setVerticalBarPadding(4, 4);
        m_sidebar->createItem = [] { return std::make_unique<SidebarRow>(); };
        m_sidebar->bindItem = [this](ui::View *item, size_t row) {
            auto *sidebar = static_cast<SidebarRow *>(item);
            const int index = int(row);
            if (index >= int(m_window.m_threads.size()))
                return;
            sidebar->bind(m_window.m_threads[size_t(index)], index == m_window.m_current);
            sidebar->onClicked = [this, index] { m_window.selectThread(index); };
        };

        m_messages->setVerticalBarPolicy(ui::ScrollView::BarPolicy::AsNeeded);
        m_messages->setVerticalBarPadding(6, 6);
        m_messages->setBarsBelowContent(false);
        m_scrollToEnd.setSingleShot(true);
        m_scrollToEnd.setInterval(std::chrono::milliseconds(30));
        m_scrollToEnd.onTimeout = [this] {
            m_messages->setScrollPosition({0, m_messages->contentSize().height});
        };

        m_title->setFontSize(ui::Theme::current().fontSize() + 3);
        m_title->setFadesWhenDisabled(false);
        m_subtitle->setFontSize(ui::Theme::current().smallFontSize());
        m_subtitle->setColor(ui::ColorId::DisabledText);
        m_subtitle->setFadesWhenDisabled(false);

        m_empty->setTextAlignment(ui::Label::TextAlignment::Center);
        m_empty->setVerticallyCentered(true);
        m_empty->setFadesWhenDisabled(false);
        m_empty->setWrap(true);

        m_composer->setPlaceholder(tr("Message"));
        m_composer->setTextMargins(12, 8);
        m_composer->setFontSize(ui::Theme::current().fontSize());
        m_composer->onAccepted = [this] { m_window.send(); };
        m_send->onClicked = [this] { m_window.send(); };

        m_status->setFontSize(ui::Theme::current().smallFontSize());
        m_status->setColor(ui::ColorId::DisabledText);
        m_status->setFadesWhenDisabled(false);
    }

    double sidebarWidth() const
    {
        return std::clamp(geometry().width * 0.30, 220.0, 300.0);
    }

    void setEmptyText(const std::string &text)
    {
        m_empty->setText(text);
        m_empty->setVisible(!text.empty());
    }

    ui::ListView *sidebar() const { return m_sidebar; }
    ui::ScrollView *messages() const { return m_messages; }

    void updateAttachment(int64_t id, const std::string &path)
    {
        if (auto *content = dynamic_cast<ConversationView *>(m_messages->contentView()))
            content->updateAttachment(id, path);
        m_viewer->updateIfShowing(id, path);
    }

    void showImage(int64_t id, const std::string &path)
    {
        m_viewer->show(id, path);
    }

    // Rebuilds the conversation; the scroll to the newest message waits for
    // the content's height to be known.
    void showMessages(const std::vector<Message> &messages, bool group)
    {
        m_messages->setContentView(std::make_unique<ConversationView>(
                messages, group, [this](int64_t id, const std::string &path) {
                    m_window.openImage(id, path);
                }));
        m_scrollToEnd.start();
    }
    ui::TextField *composer() const { return m_composer; }
    SendButton *sendButton() const { return m_send; }
    ui::Label *title() const { return m_title; }
    ui::Label *subtitle() const { return m_subtitle; }
    ui::Label *status() const { return m_status; }
    ui::Label *empty() const { return m_empty; }

protected:
    void paint(ui::Painter &painter) override
    {
        const ui::Theme &theme = ui::Theme::current();
        const double kSidebar = sidebarWidth();
        // The window body is rounded by the compositor; clip what we paint to
        // the same shape so no fill shows past it at the corners.
        painter.clipRoundedRect({-1, -1, geometry().width + 2, geometry().height + 2}, 25);
        // The conversation is the window's paper; the list sits on a quieter
        // second surface, as iMessage's sidebar does.
        painter.fillRect({kSidebar, 0, geometry().width - kSidebar, geometry().height},
                         theme.darkMode ? ui::Color::rgb(0x1C1C1E).withAlpha(0.88)
                                        : ui::Color::rgb(0xFFFFFF).withAlpha(0.90));
        painter.fillRect({0, 0, kSidebar, geometry().height},
                         theme.darkMode ? ui::Color::rgb(0xFFFFFF).withAlpha(0.06)
                                        : ui::Color::rgb(0xFFFFFF).withAlpha(0.30));
        painter.fillRoundedRect({kSidebar - 0.5, 12, 1, geometry().height - 24}, 0.5,
                                theme.darkMode ? ui::Color::rgb(0xFFFFFF).withAlpha(0.10)
                                               : ui::Color::rgb(0x000000).withAlpha(0.09));
        // The composer's pill, as iMessage's field floats in one.
        const double composerTop = geometry().height - 28 - 56;
        painter.fillRoundedRect({kSidebar + 14, composerTop, geometry().width - kSidebar - 28, 46}, 23,
                                theme.darkMode ? ui::Color::rgb(0xFFFFFF).withAlpha(0.07)
                                               : ui::Color::rgb(0xFFFFFF).withAlpha(0.55));
    }

    void layout() override
    {
        const double width = geometry().width;
        const double height = geometry().height;
        const double kSidebar = sidebarWidth();
        constexpr double kStatus = 28;
        constexpr double kComposer = 56;

        // The window's glass covers the whole body, whatever size it is now.
        m_window.setBackgroundBlur({0, 0, width, height}, 24);
        m_header->setGeometry({0, 0, width, 44});
        m_viewer->setGeometry({0, 0, width, height});
        m_sidebarTitle->setGeometry({124, 11, std::max(0.0, kSidebar - 140), 22});
        m_sidebar->setGeometry({0, 44, kSidebar, height - kStatus - 44});
        const double left = kSidebar;
        const double right = width - kSidebar;
        m_title->setGeometry({left + 18, 10, std::max(0.0, right - 140), 24});
        m_subtitle->setGeometry({left + 18, 32, std::max(0.0, right - 140), 12});
        const double listTop = 52;
        const double listBottom = height - kStatus - kComposer - 8;
        m_messages->setGeometry({left + 8, listTop, std::max(0.0, right - 24),
                                 std::max(0.0, listBottom - listTop)});
        m_empty->setGeometry({left + 24, listTop, std::max(0.0, right - 48),
                              std::max(0.0, listBottom - listTop)});
        m_composer->setGeometry({left + 16, height - kStatus - kComposer + 10,
                                 std::max(0.0, right - 74), 36});
        m_send->setGeometry({width - 56, height - kStatus - kComposer + 12, 32, 32});
        m_status->setGeometry({left + 18, height - kStatus + 4, std::max(0.0, right - 36), 20});
    }

private:
    MessagesWindow &m_window;
    ui::WindowHeader *m_header;
    ui::Label *m_sidebarTitle;
    ui::ListView *m_sidebar;
    ui::Label *m_title;
    ui::Label *m_subtitle;
    ui::ScrollView *m_messages;
    base::Timer m_scrollToEnd{base::EventLoop::current()};
    ui::Label *m_empty;
    ui::TextField *m_composer;
    SendButton *m_send;
    ui::Label *m_status;
    ImageViewer *m_viewer;
};

MessagesWindow::MessagesWindow(base::EventLoop *loop, bool demo)
    : m_refresh(loop)
    , m_demo(demo)
    , m_client(std::make_unique<MessagesClient>(loop))
{
    setTitle(tr("Messages"));
    resize({920, 620});

    // Tahoe's liquid glass: the compositor blurs what lies behind the window,
    // and the material tints, saturates and lights it. Everything the app
    // paints goes over that.
    const ui::Theme &theme = ui::Theme::current();
    ui::Color tint = theme.darkMode ? ui::Color::rgb(0x1C1C1E) : ui::Color::rgb(0xFFFFFF);
    setBackgroundColor(ui::ColorRecipe(tint.withAlpha(0.0)));
    setBackgroundBlur({0, 0, 920, 620}, 24);
    ui::GlassMaterial material;
    material.blurSigma = 8;
    material.saturation = 1.35;
    material.specularIntensity = 0.55;
    material.shininess = 60;
    material.sheen = 0.35;
    material.tintRed = tint.r / 255.0;
    material.tintGreen = tint.g / 255.0;
    material.tintBlue = tint.b / 255.0;
    material.tintAlpha = theme.darkMode ? 0.50 : 0.46;
    material.bevel = 24;
    material.rim = 0.35;
    setBackgroundGlass(material);

    m_root = new RootView(*this);
    setContentView(std::unique_ptr<ui::View>(m_root));

    if (m_demo) {
        Thread first;
        first.key = "demo:1";
        first.name = "Avery";
        first.lastTimestamp = "2026-11-22T10:32:00";
        first.messages = {{"1", "Are we still on for lunch?", "2026-11-22T10:30:00", "", false, true},
                          {"2", "Yes — the usual place?", "2026-11-22T10:31:00", "", true, true},
                          {"3", "Perfect. See you at one.", "2026-11-22T10:32:00", "", false, false},
                          {"4", "The menu is at https://example.com/menu if you want to look first.",
                           "2026-11-22T10:33:00", "", false, true}};
        first.snippet = first.messages.back().body;
        first.unread = 1;
        Thread second;
        second.key = "demo:2";
        second.name = "Rivera family";
        second.group = true;
        second.lastTimestamp = "2026-11-21T18:04:00";
        second.messages = {{"4", "Dinner on Sunday?", "2026-11-21T18:04:00", "Mum", false, true}};
        second.snippet = second.messages.back().body;
        m_threads = {first, second};
    } else {
        std::string phoneError;
        m_phone = std::make_unique<phone::PhoneLink>(loop);
        if (m_phone->start(phoneError)) {
            m_usePhone = true;
            m_phoneSubscriptions.push_back(
                    m_phone->stateChanged.connect([this] { updateStatus(); }));
            m_refresh.setSingleShot(true);
            m_refresh.setInterval(std::chrono::milliseconds(60));
            m_refresh.onTimeout = [this] {
                phoneConversations(std::move(m_pendingConversations));
            };
            m_phoneSubscriptions.push_back(m_phone->conversationsReceived.connect(
                    [this](std::vector<phone::Conversation> list) {
                        m_pendingConversations = std::move(list);
                        m_refresh.start();
                    }));
            m_phoneSubscriptions.push_back(m_phone->threadReceived.connect(
                    [this](int64_t thread, std::vector<phone::SmsMessage> messages) {
                        phoneMessages(thread, std::move(messages));
                    }));
            m_phoneSubscriptions.push_back(m_phone->attachmentReady.connect(
                    [this](int64_t id, std::string path) {
                        phoneAttachment(id, std::move(path));
                    }));
            m_phoneSubscriptions.push_back(m_phone->notice.connect(
                    [this](std::string text) { m_root->status()->setText(text); }));
            m_phoneSubscriptions.push_back(
                    m_phone->contactsChanged.connect([this] { reloadFromPhone(); }));
            reloadFromPhone();
            updateStatus();
        } else {
            m_phone.reset();
            m_subscriptions.push_back(m_client->changed.connect([this] {
                reloadThreads();
                updateStatus();
            }));
            reloadThreads();
            updateStatus();
        }
    }

    m_root->sidebar()->setRowCount(m_threads.size());
    m_root->sidebar()->reload();
    if (!m_threads.empty())
        selectThread(0);
    else
        m_root->setEmptyText(tr("No conversations yet."));
}

MessagesWindow::~MessagesWindow()
{
    m_subscriptions.clear();
}

void MessagesWindow::reloadThreads()
{
    if (m_demo)
        return;
    const std::string selected = m_current >= 0 && m_current < int(m_threads.size())
            ? m_threads[size_t(m_current)].key
            : std::string();
    m_threads = m_client->threads();
    m_root->sidebar()->setRowCount(m_threads.size());
    m_root->sidebar()->reload();
    for (int index = 0; index < int(m_threads.size()); ++index) {
        if (m_threads[size_t(index)].key == selected) {
            selectThread(index);
            return;
        }
    }
    if (m_current >= int(m_threads.size()))
        m_current = -1;
    if (!m_threads.empty() && m_current < 0)
        selectThread(0);
    if (m_threads.empty())
        m_root->setEmptyText(tr("No conversations yet. Messages you receive on the iPhone appear here."));
}

void MessagesWindow::reloadFromPhone()
{
    // Newest first, whatever order the summaries arrived in or were updated.
    std::sort(m_conversations.begin(), m_conversations.end(),
              [](const phone::Conversation &left, const phone::Conversation &right) {
                  return left.date > right.date;
              });
    const int64_t selected = m_current >= 0 && m_current < int(m_threads.size())
            ? m_threads[size_t(m_current)].threadId
            : 0;
    m_threads.clear();
    for (const phone::Conversation &conversation : m_conversations) {
        Thread thread;
        thread.key = "phone:" + std::to_string(conversation.threadId);
        thread.threadId = conversation.threadId;
        thread.address = conversation.address;
        thread.name = m_phone ? m_phone->nameFor(conversation.address) : std::string();
        if (thread.name.empty())
            thread.name = prettyAddress(conversation.address);
        thread.snippet = conversation.snippet;
        thread.lastTimestamp = timestampOf(conversation.date);
        thread.unread = conversation.read ? 0 : 1;
        const auto cached = m_phoneThreads.find(conversation.threadId);
        if (cached != m_phoneThreads.end()) {
            for (const phone::SmsMessage &sms : cached->second)
                thread.messages.push_back(messageOf(sms));
        }
        m_threads.push_back(std::move(thread));
    }

    m_root->sidebar()->setRowCount(m_threads.size());
    m_root->sidebar()->reload();

    for (int index = 0; index < int(m_threads.size()); ++index) {
        if (m_threads[size_t(index)].threadId != selected)
            continue;
        if (index == m_current) {
            m_root->title()->setText(m_threads[size_t(index)].name);
            m_root->sidebar()->reload();
            return;
        }
        selectThread(index);
        return;
    }
    m_current = -1;
    if (!m_threads.empty()) {
        selectThread(0);
        return;
    }
    m_root->setEmptyText(m_phone && m_phone->isConnected()
                                 ? tr("No messages yet.")
                                 : tr("Your phone connects over Wi-Fi. Keep KDE Connect open on it."));
}

void MessagesWindow::phoneConversations(std::vector<phone::Conversation> conversations)
{
    std::sort(conversations.begin(), conversations.end(),
              [](const phone::Conversation &left, const phone::Conversation &right) {
                  return left.date > right.date;
              });
    m_conversations = std::move(conversations);
    reloadFromPhone();
}

void MessagesWindow::phoneMessages(int64_t threadId, std::vector<phone::SmsMessage> messages)
{
    std::sort(messages.begin(), messages.end(),
              [](const phone::SmsMessage &left, const phone::SmsMessage &right) {
                  return left.date < right.date;
              });
    if (m_requestedThread == threadId)
        m_requestedThread = 0;
    m_phoneThreads[threadId] = std::move(messages);
    if (m_current >= 0 && m_current < int(m_threads.size())
        && m_threads[size_t(m_current)].threadId == threadId) {
        Thread &thread = m_threads[size_t(m_current)];
        thread.messages.clear();
        for (const phone::SmsMessage &sms : m_phoneThreads[threadId])
            thread.messages.push_back(messageOf(sms));
        const std::string last = thread.messages.empty()
                ? std::string()
                : thread.messages.back().timestamp + thread.messages.back().body;
        if (m_shownThread != thread.threadId || m_shownCount != thread.messages.size()
            || m_shownLast != last) {
            m_shownThread = thread.threadId;
            m_shownCount = thread.messages.size();
            m_shownLast = last;
            m_root->showMessages(thread.messages, thread.group);
        }
        m_root->setEmptyText(thread.messages.empty() ? tr("Loading…") : std::string());
    }
}

void MessagesWindow::openImage(int64_t messageId, const std::string &path)
{
    bool full = false;
    for (const auto &[threadId, list] : m_phoneThreads) {
        for (const phone::SmsMessage &sms : list) {
            if (sms.id == messageId)
                full = !sms.fullPath.empty();
        }
    }
    // An old preview: ask for the original, and the viewer sharpens when it
    // lands rather than making the click wait.
    if (!full && m_usePhone && m_phone)
        m_phone->requestAttachmentNow(messageId);
    m_root->showImage(messageId, path);
}

void MessagesWindow::phoneAttachment(int64_t messageId, std::string path)
{
    for (auto &[threadId, list] : m_phoneThreads) {
        for (phone::SmsMessage &sms : list) {
            if (sms.id == messageId)
                sms.fullPath = path;
        }
    }
    if (m_current >= 0 && m_current < int(m_threads.size())) {
        for (Message &message : m_threads[size_t(m_current)].messages) {
            if (message.id == messageId)
                message.attachmentPath = path;
        }
    }
    m_root->updateAttachment(messageId, path);
}

void MessagesWindow::selectThread(int index)
{
    if (index < 0 || index >= int(m_threads.size()))
        return;
    m_current = index;
    Thread &thread = m_threads[size_t(index)];
    m_root->title()->setText(thread.name);
    m_root->subtitle()->setText(thread.group ? tr("Group conversation") : std::string());
    const std::string last = thread.messages.empty()
            ? std::string()
            : thread.messages.back().timestamp + thread.messages.back().body;
    if (m_shownThread != thread.threadId || m_shownCount != thread.messages.size()
        || m_shownLast != last) {
        m_shownThread = thread.threadId;
        m_shownCount = thread.messages.size();
        m_shownLast = last;
        m_root->showMessages(thread.messages, thread.group);
    }
    m_root->sidebar()->reload();
    m_root->setEmptyText(thread.messages.empty() ? tr("Loading…") : std::string());
    const bool canReply = m_demo || m_usePhone || (!thread.group && thread.replyReady);
    m_root->composer()->setEnabled(canReply);
    m_root->sendButton()->setEnabled(canReply);
    if (canReply)
        m_root->composer()->setPlaceholder(tr("Message"));
    else
        m_root->composer()->setPlaceholder(tr("Group replies are not in this version yet"));
    if (m_usePhone) {
        if (thread.messages.empty() && thread.threadId != 0 && m_phone
            && m_requestedThread != thread.threadId) {
            m_requestedThread = thread.threadId;
            m_phone->requestThread(thread.threadId);
        }
    } else if (!m_demo) {
        m_client->markRead(thread);
    }
}

void MessagesWindow::send()
{
    if (m_current < 0 || m_current >= int(m_threads.size()))
        return;
    Thread &thread = m_threads[size_t(m_current)];
    const std::string body = m_root->composer()->text();
    std::fprintf(stderr, "send: thread %d, %zu character(s)\n", m_current, body.size());
    if (body.empty())
        return;
    if (m_demo) {
        thread.messages.push_back({"", body, "", "", true, true});
        thread.snippet = body;
        m_root->composer()->setText({});
        selectThread(m_current);
        return;
    }
    if (m_usePhone) {
        std::string error;
        if (!m_phone->sendSms(thread.address, body, error)) {
            m_root->status()->setText(error);
            return;
        }
        phone::SmsMessage sent;
        sent.threadId = thread.threadId;
        sent.address = thread.address;
        sent.body = body;
        sent.date = nowMilliseconds();
        sent.type = 2;
        sent.read = true;
        m_phoneThreads[thread.threadId].push_back(sent);
        thread.messages.push_back(messageOf(sent));
        thread.snippet = body;
        thread.lastTimestamp = timestampOf(sent.date);
        // The list follows the phone: this conversation is now the newest.
        for (phone::Conversation &conversation : m_conversations) {
            if (conversation.threadId == thread.threadId) {
                conversation.date = sent.date;
                conversation.snippet = body;
                conversation.type = 2;
            }
        }
        m_root->composer()->setText({});
        reloadFromPhone();
        return;
    }
    std::string error;
    if (!m_client->reply(thread, body, error)) {
        m_root->status()->setText(error);
        return;
    }
    m_root->composer()->setText({});
    reloadThreads();
}

void MessagesWindow::updateStatus()
{
    if (m_demo) {
        m_root->status()->setText(tr("Demo conversations"));
        return;
    }
    if (m_usePhone) {
        if (!m_phone->isConnected()) {
            m_root->status()->setText(tr("Waiting for your phone. Keep KDE Connect open."));
            if (m_threads.empty())
                m_root->setEmptyText(
                        tr("Your phone connects over Wi-Fi. Keep KDE Connect open on it."));
            return;
        }
        std::string text = m_phone->phoneName().empty() ? tr("Phone") : m_phone->phoneName();
        if (m_phone->battery() >= 0)
            text += " · " + std::to_string(m_phone->battery()) + "%";
        if (!m_phone->isPaired()) {
            text += " · ";
            text += tr("confirm the pairing on your phone");
        }
        m_root->status()->setText(text);
        return;
    }
    const Status status = m_client->status();
    if (!status.running) {
        m_root->status()->setText(
                tr("BlueFerry is not running. Pair your iPhone with it to see Messages."));
        m_root->setEmptyText(tr("Pair your iPhone with BlueFerry, then reopen Messages."));
        return;
    }
    if (!status.connected) {
        m_root->status()->setText(tr("Waiting for the iPhone."));
        return;
    }
    std::string text = status.phone.empty() ? tr("iPhone") : status.phone;
    if (status.battery >= 0)
        text += " · " + std::to_string(status.battery) + "%";
    m_root->status()->setText(text);
    if (m_threads.empty())
        m_root->setEmptyText(tr("No conversations yet. Messages you receive on the iPhone appear here."));
}

} // namespace cutefish::messages
