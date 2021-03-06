#include <vtextedit/vtextedit.h>

#include <QTextBlock>
#include <QResizeEvent>
#include <QWheelEvent>
#include <QRegularExpression>
#include <QDebug>
#include <QKeyEvent>
#include <QTextLayout>
#include <QTimer>
#include <QAbstractTextDocumentLayout>
#include <QScrollBar>
#include <QGuiApplication>
#include <QMenu>
#include <QInputMethod>

#include <inputmode/abstractinputmode.h>
#include <vtextedit/texteditutils.h>

#include "scrollbar.h"

using namespace vte;

VTextEdit::VTextEdit(QWidget *p_parent)
    : QTextEdit(p_parent)
{
    // Use custom vertical scrollbar.
    setVerticalScrollBar(new ScrollBar(this));

    installEventFilter(this);

    m_cursorPositionChangeTime.start();
    connect(this, &QTextEdit::cursorPositionChanged,
            this, &VTextEdit::handleCursorPositionChange);

    connect(this->document(), &QTextDocument::contentsChanged,
            this, [this]() {
                // Only emit the contensChanged signal when there is modification to the contents.
                // Begin and end an edit block without other operation will result in a fake contentsChanged.
                // Rehighlighting a block will also result in a fake contentsChanged.
                if (document()->revision() == m_lastDocumentRevisionWithChanges) {
                    emit contentsChanged();
                }
            });

    connect(this->document(), &QTextDocument::contentsChange,
            this, [this](int p_position, int p_charsRemoved, int p_charsAdded) {
                Q_UNUSED(p_position);
                if (p_charsRemoved != 0 || p_charsAdded != 0) {
                    m_lastDocumentRevisionWithChanges = document()->revision();
                }
            });

    // ATTENTION: we rely on the Qt fact that this slot will be called before
    // all other slots connected to selectionChanged.
    connect(this, &QTextEdit::selectionChanged,
            this, [this]() {
                auto cursor = textCursor();
                if (cursor.hasSelection()) {
                    m_selections.m_selection = Selection(cursor.position(), cursor.anchor());
                } else {
                    m_selections.m_selection.clear();
                }
            });

    m_updateCursorWidthTimer = new QTimer(this);
    m_updateCursorWidthTimer->setSingleShot(true);
    m_updateCursorWidthTimer->setInterval(c_updateCursorWidthTimerInterval);
    connect(m_updateCursorWidthTimer, &QTimer::timeout,
            this, &VTextEdit::updateCursorWidthToNextChar);
}

void VTextEdit::handleCursorPositionChange()
{
    // Qt BUG: In the case of exist of invislbe blocks, when a user click at the top
    // of the document, the hit test of underlying document layout may treat the cursor
    // at wrong blocks (such as those invisible blocks or the next block to those invisible blocks).
    // TODO: We could do something tricky to fix it here if necessary.
    const int intervalThreshold = 50;
    const int interval = m_cursorPositionChangeTime.restart();

    auto cursor = textCursor();
    auto tb = cursor.block();
    if (!tb.isVisible()) {
        // When cursor is placed in a invisible block. We move it
        // to the first block that is visible.
        m_cursorLine = tb.blockNumber();
        tb = document()->firstBlock();
        while (tb.isValid() && !tb.isVisible()) {
            tb = tb.next();
        }

        Q_ASSERT(tb.isValid());
        cursor.setPosition(tb.position());
        setTextCursor(cursor);
        return;
    }

    // Update the cursor width if we draw cursor as block.
    if (m_drawCursorAsBlock != DrawCursorAsBlock::None) {
        // Avoid flicker in user interaction.
        if (interval >= intervalThreshold) {
            m_updateCursorWidthTimer->start(0);
        } else {
            m_updateCursorWidthTimer->start(c_updateCursorWidthTimerInterval);
        }
    }

    int line = tb.blockNumber();
    if (line != m_cursorLine) {
        m_cursorLine = line;
        emit cursorLineChanged();
    }

    checkCenterCursor();
}

void VTextEdit::resizeEvent(QResizeEvent *p_event)
{
    QTextEdit::resizeEvent(p_event);
    emit resized();
}

void VTextEdit::wheelEvent(QWheelEvent *p_event)
{
    if (p_event->modifiers() & Qt::ControlModifier) {
        // Let editor to zoom.
        return;
    }
    QTextEdit::wheelEvent(p_event);
}

void VTextEdit::mousePressEvent(QMouseEvent *p_event)
{
    QTextEdit::mousePressEvent(p_event);
}

void VTextEdit::mouseReleaseEvent(QMouseEvent *p_event)
{
    QTextEdit::mouseReleaseEvent(p_event);
    emit mouseReleased(p_event);
}

static QTextDocument::FindFlags findFlagsToDocumentFindFlags(FindFlags p_flags)
{
    QTextDocument::FindFlags findFlags = 0;

    if (p_flags & FindFlag::FindBackward) {
        findFlags |= QTextDocument::FindBackward;
    }

    if (p_flags & FindFlag::CaseSensitive) {
        findFlags |= QTextDocument::FindCaseSensitively;
    }

    if (p_flags & FindFlag::WholeWordOnly) {
        findFlags |= QTextDocument::FindWholeWords;
    }

    return findFlags;
}

QList<QTextCursor> VTextEdit::findAllText(const QString &p_text,
                                          FindFlags p_flags,
                                          int p_start,
                                          int p_end)
{
    if (p_text.isEmpty() || (p_start >= p_end && p_end >= 0)) {
        return QList<QTextCursor>();
    }

    auto flags = findFlagsToDocumentFindFlags(p_flags);
    if (p_flags & FindFlag::RegularExpression) {
        QRegularExpression regex(p_text);
        if (!regex.isValid()) {
            return QList<QTextCursor>();
        }
        return findAllTextInDocument(regex, flags, p_start, p_end);
    } else {
        return findAllTextInDocument(p_text, flags, p_start, p_end);
    }
}

QTextCursor VTextEdit::findText(const QString &p_text,
                                FindFlags p_flags,
                                int p_start)
{
    if (p_text.isEmpty()) {
        return QTextCursor();
    }

    auto flags = findFlagsToDocumentFindFlags(p_flags);
    if (p_flags & FindFlag::RegularExpression) {
        QRegularExpression regex(p_text);
        if (!regex.isValid()) {
            return QTextCursor();
        }
        return findTextInDocument(regex, flags, p_start);
    } else {
        return findTextInDocument(p_text, flags, p_start);
    }
}

void VTextEdit::setInputMode(const QSharedPointer<AbstractInputMode> &p_mode)
{
    Q_ASSERT(p_mode != m_inputMode);
    if (m_inputMode) {
        m_inputMode->deactivate();
    }

    m_inputMode = p_mode;
    if (m_inputMode) {
        m_inputMode->activate();
    }
}

QSharedPointer<AbstractInputMode> VTextEdit::getInputMode() const
{
    return m_inputMode;
}

void VTextEdit::keyPressEvent(QKeyEvent *p_event)
{
    if (m_inputMode && m_inputMode->handleKeyPress(p_event)) {
        return;
    }

    if (m_inputMode) {
        m_inputMode->preKeyPressDefaultHandle(p_event);
    }

    handleDefaultKeyPress(p_event);

    if (m_inputMode) {
        m_inputMode->postKeyPressDefaultHandle(p_event);
    }
}

void VTextEdit::handleDefaultKeyPress(QKeyEvent *p_event)
{
    const int key = p_event->key();
    const int modifiers = p_event->modifiers();

    bool isHandled = false;
    switch (key) {
    case Qt::Key_Tab:
        isHandled = handleKeyTab(p_event);
        break;

    case Qt::Key_Backtab:
        isHandled = handleKeyBackTab(p_event);
        break;

    case Qt::Key_Return:
        if (modifiers == Qt::ShiftModifier) {
            // Shift+Return by default will insert a soft line within a block.
            // It will complicate things. Disable it by default.
            p_event->accept();
            isHandled = true;
        }
        break;

    default:
        break;
    }

    if (!isHandled) {
        QTextEdit::keyPressEvent(p_event);
    }
}

bool VTextEdit::eventFilter(QObject *p_obj, QEvent *p_event)
{
    switch (p_event->type()) {
    case QEvent::ShortcutOverride:
    {
        // This event is sent when a shortcut is about to trigger.
        // If the override event is accepted, the event is delivered
        // as a normal key press to the focus widget.
        QKeyEvent *ke = static_cast<QKeyEvent *>(p_event);
        if (m_inputMode && m_inputMode->stealShortcut(ke)) {
            ke->accept();
            return true;
        }

        break;
    }

    default:
        break;
    }

    return QTextEdit::eventFilter(p_obj, p_event);
}

void VTextEdit::setDrawCursorAsBlock(bool p_enabled, bool p_half)
{
    DrawCursorAsBlock newVal = DrawCursorAsBlock::None;
    if (p_enabled) {
        newVal = p_half ? DrawCursorAsBlock::Half : DrawCursorAsBlock::Full;
    }

    if (newVal == m_drawCursorAsBlock) {
        return;
    }

    m_updateCursorWidthTimer->stop();
    m_drawCursorAsBlock = newVal;
    if (m_drawCursorAsBlock != DrawCursorAsBlock::None) {
        updateCursorWidthToNextChar();
    } else {
        setCursorWidth(1);
        repaintBlock(textCursor().block());
    }
}

void VTextEdit::updateCursorWidthToNextChar()
{
    if (m_drawCursorAsBlock == DrawCursorAsBlock::None) {
        return;
    }

    int wid = 4;
    auto cursor = textCursor();
    auto block = cursor.block();
    if (!cursor.atBlockEnd()) {
#if 0
        // Calculate the char width via QFontMetrics.
        cursor.clearSelection();
        auto block = cursor.block();
        const QChar ch = block.text().at(cursor.positionInBlock());

        // Move cursor one step since charFormat() returns
        // the format of the character immediately before the cursor position().
        bool ret = cursor.movePosition(QTextCursor::NextCharacter);
        Q_ASSERT(ret);
        auto fmt = cursor.charFormat();
        QFontMetrics fm(fmt.font());
        wid = fm.horizontalAdvance(ch);
#endif
        // Calculate the char width via QTextLayout.
        bool succeed = false;
        const auto layout = block.layout();
        if (layout) {
            const int pib = cursor.positionInBlock();
            auto line = layout->lineForTextPosition(pib);
            if (line.isValid()) {
                wid = qAbs(line.cursorToX(pib + 1) - line.cursorToX(pib));
                succeed = true;
            }
        }

        if (!succeed) {
            // May be this block is not layouted yet.
            m_updateCursorWidthTimer->start(c_updateCursorWidthTimerInterval);
            return;
        }
    }

    if (m_drawCursorAsBlock == DrawCursorAsBlock::Half) {
        wid = qMax(1, (int)(wid * 1.0 / 2));
    }

    const int currentWidth = cursorWidth();
    if (wid != currentWidth) {
        // We need to manually update the larger rect to clean up the shadow when width
        // becomes smaller.
        const bool forceUpdate = wid < currentWidth;

        setCursorWidth(wid);

        if (forceUpdate) {
            repaintBlock(block);
        }
    }
}

void VTextEdit::repaintBlock(const QTextBlock &p_block)
{
    emit document()->documentLayout()->updateBlock(p_block);
}

const VTextEdit::Selections &VTextEdit::getSelections() const
{
    return m_selections;
}

const VTextEdit::Selection &VTextEdit::getSelection() const
{
    return m_selections.getSelection();
}

bool VTextEdit::hasSelection() const
{
    return getSelection().isValid();
}

QString VTextEdit::selectedText() const
{
    const auto &selection = m_selections.getSelection();
    return getSelectedText(selection);
}

QString VTextEdit::getSelectedText(const Selection &p_selection) const
{
    if (!p_selection.isValid()) {
        return QString();
    }

    auto cursor = textCursor();
    cursor.setPosition(p_selection.start());
    cursor.setPosition(p_selection.end(), QTextCursor::KeepAnchor);
    return cursor.selectedText();
}

void VTextEdit::setOverriddenSelection(int p_start, int p_end)
{
    Selection newSelection(p_start, p_end);
    if (newSelection == m_selections.m_overriddenSelection) {
        return;
    }

    m_selections.m_overriddenSelection = newSelection;
    emit selectionChanged();
}

void VTextEdit::clearOverriddenSelection()
{
    if (m_selections.m_overriddenSelection.isValid()) {
        m_selections.m_overriddenSelection.clear();
        emit selectionChanged();
    }
}

QString VTextEdit::getTextByRange(int p_start, int p_end) const
{
    auto doc = document();

    p_start = qMax(0, p_start);
    p_end = qMin(p_end, doc->characterCount());

    QString res;
    while (p_start < p_end) {
        res += doc->characterAt(p_start);
        ++p_start;
    }

    return res;
}

void VTextEdit::updateCursorWidth()
{
    m_updateCursorWidthTimer->start(c_updateCursorWidthTimerInterval);
}

void VTextEdit::setCursorWidth(int p_width)
{
    QTextEdit::setCursorWidth(p_width);
    emit cursorWidthChanged();
}

void VTextEdit::checkCenterCursor()
{
    const bool mouseButtonDown = QGuiApplication::mouseButtons() != Qt::NoButton;
    if (mouseButtonDown || m_centerCursor == CenterCursor::NeverCenter) {
        return;
    }

    auto vbar = verticalScrollBar();
    if (!vbar || vbar->maximum() == vbar->minimum()) {
        return;
    }

    QRect cRect = this->cursorRect();
    QRect vRect = viewport()->rect();
    int targetCenter = vRect.center().y();
    if (m_centerCursor == CenterCursor::CenterOnBottom) {
        targetCenter = vRect.height() / 4 * 3;
        if (cRect.center().y() < targetCenter
            && cRect.top() > 0) {
            // No need to center the cursor.
            return;
        }
    }

    int offset = targetCenter - cRect.center().y();
    vbar->setValue(vbar->value() - offset);
}

void VTextEdit::setCenterCursor(CenterCursor p_centerCursor)
{
    if (m_centerCursor != p_centerCursor) {
        m_centerCursor = p_centerCursor;
        checkCenterCursor();
    }
}

void VTextEdit::setExpandTab(bool p_enable)
{
    m_expandTab = p_enable;
}

void VTextEdit::setTabStopWidthInSpaces(int p_spaces)
{
    Q_ASSERT(p_spaces > 0);
    m_tabStopWidthInSpaces = p_spaces;
    setTabStopDistance(QFontMetrics(font()).width(QLatin1Char(' ')) * m_tabStopWidthInSpaces);
}

bool VTextEdit::handleKeyTab(QKeyEvent *p_event)
{
    if (p_event->modifiers() == Qt::NoModifier) {
        bool shouldIndentBlock = false;
        auto cursor = textCursor();
        if (cursor.hasSelection()) {
            shouldIndentBlock = true;
        } else {
            auto block = cursor.block();
            if (!TextEditUtils::isEmptyBlock(block)
                && TextEditUtils::fetchIndentation(block) == cursor.positionInBlock()) {
                // Cursor at the first non-space of block.
                shouldIndentBlock = true;
            }
        }

        if (shouldIndentBlock) {
            TextEditUtils::indentBlocks(this, !m_expandTab, m_tabStopWidthInSpaces, true);
        } else {
            // Insert Tab or spaces.
            if (m_expandTab) {
                cursor.insertText(QString(m_tabStopWidthInSpaces, QLatin1Char(' ')));
            } else {
                cursor.insertText(QStringLiteral("\t"));
            }
        }

        return true;
    }

    return false;
}

bool VTextEdit::handleKeyBackTab(QKeyEvent *p_event)
{
    if (p_event->modifiers() == Qt::ShiftModifier) {
        TextEditUtils::indentBlocks(this, !m_expandTab, m_tabStopWidthInSpaces, false);
        return true;
    }

    return false;
}

bool VTextEdit::canInsertFromMimeData(const QMimeData *p_source) const
{
    bool allowed = false;
    bool handled = false;
    emit const_cast<VTextEdit *>(this)->canInsertFromMimeDataRequested(p_source, &handled, &allowed);
    if (!handled) {
        return QTextEdit::canInsertFromMimeData(p_source);
    } else {
        return allowed;
    }
}

void VTextEdit::insertFromMimeData(const QMimeData *p_source)
{
    bool handled = false;
    emit insertFromMimeDataRequested(p_source, &handled);

    if (!handled) {
        QTextEdit::insertFromMimeData(p_source);
    }
}

void VTextEdit::insertFromMimeDataOfBase(const QMimeData *p_source)
{
    QTextEdit::insertFromMimeData(p_source);
}

void VTextEdit::contextMenuEvent(QContextMenuEvent *p_event)
{
    bool handled = false;
    QScopedPointer<QMenu> menu;
    emit contextMenuEventRequested(p_event, &handled, &menu);
    if (handled) {
        p_event->accept();
        if (menu) {
            menu->exec(p_event->globalPos());
        }
    } else {
        QTextEdit::contextMenuEvent(p_event);
    }
}

QVariant VTextEdit::inputMethodQuery(Qt::InputMethodQuery p_query) const
{
    if (p_query == Qt::ImEnabled) {
        return m_inputMethodEnabled;
    }

    return QTextEdit::inputMethodQuery(p_query);
}

void VTextEdit::setInputMethodEnabled(bool p_enabled)
{
    if (m_inputMethodEnabled != p_enabled) {
        m_inputMethodEnabled = p_enabled;

        QInputMethod *im = QGuiApplication::inputMethod();
        im->reset();
        // Ask input method to query current state, which will call inputMethodQuery().
        im->update(Qt::ImEnabled);
    }
}
