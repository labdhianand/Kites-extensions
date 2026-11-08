#include "optimizer_editor.h"

#include "language_service/language_service.h"
#include "ui/common/line_number_gutter_column.h"
#include "ui/common/syntax_highlighter.h"
#include "ui/theme/theme_manager.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QKeyEvent>
#include <QScrollBar>
#include <QStringListModel>
#include <QTextBlock>
#include <QTextEdit>

namespace Kites
{
namespace
{
// Marks the extra selections that are our tints, so they can be told apart from the base
// editor's current-line highlight.
constexpr int kTintProperty = QTextFormat::UserProperty + 1;

bool isCompletionWordChar(QChar c)
{
    // same word characters as the Editor tab: instructions like fadd.s, labels with '.' or '$'
    return c.isLetterOrNumber() || c == '_' || c == '.' || c == '$';
}
} // namespace

OptimizerEditor::OptimizerEditor(Tint tint, bool editable, QWidget *parent)
    : KitesEditor(parent), m_tint(tint)
{
    m_syntaxHighlighter = new SyntaxHighlighter(document());
    setReadOnly(!editable);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    addLeftGutterColumn(new LineNumberGutterColumn(this));
    updateViewPortMargins();

    if (editable)
    {
        m_completer = new QCompleter(this);
        m_completer->setModel(new QStringListModel(m_completer));
        m_completer->setCaseSensitivity(Qt::CaseInsensitive);
        m_completer->setCompletionMode(QCompleter::PopupCompletion);
        m_completer->setWidget(this);
        connect(m_completer, QOverload<const QString &>::of(&QCompleter::activated), this,
                &OptimizerEditor::insertCompletion);
    }

    // The base editor replaces the extra selections with the current-line highlight whenever the
    // cursor moves or the theme changes; these run after it and add the tints back.
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &OptimizerEditor::applyTints);
    connect(&ThemeManager::getInstance(), &ThemeManager::editorThemeChangedSignal, this,
            &OptimizerEditor::applyTints);
    applyTints();
}

void OptimizerEditor::showProgram(const QString &text, const std::vector<int> &changedLines)
{
    m_changedLines = changedLines;
    setPlainText(text);
    applyTints();
}

void OptimizerEditor::setTintedLines(const std::vector<int> &changedLines)
{
    m_changedLines = changedLines;
    applyTints();
}

void OptimizerEditor::changeEvent(QEvent *event)
{
    KitesEditor::changeEvent(event); // may redo the current-line highlight
    if (event->type() == QEvent::PaletteChange)
    {
        applyTints();
    }
}

void OptimizerEditor::applyTints()
{
    // Keep whatever the base editor put there (the current-line highlight), drop old tints. With
    // no text, drop the highlight too: it would be painted over the placeholder text.
    QList<QTextEdit::ExtraSelection> selections;
    const bool empty = document()->isEmpty();
    for (const QTextEdit::ExtraSelection &selection : extraSelections())
    {
        if (!empty && !selection.format.hasProperty(kTintProperty))
        {
            selections.append(selection);
        }
    }

    const ThemeManager &theme = ThemeManager::getInstance();
    QColor color =
        m_tint == Tint::Added ? theme.getEditorSuccessColor() : theme.getEditorErrorColor();
    color.setAlpha(48); // a soft wash that keeps the syntax colours readable in every theme

    // Tints go first so the current-line highlight is drawn over them.
    QList<QTextEdit::ExtraSelection> tints;
    for (int line : m_changedLines)
    {
        const QTextBlock block = document()->findBlockByNumber(line - 1);
        if (!block.isValid())
        {
            continue;
        }
        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(color);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);
        selection.format.setProperty(kTintProperty, true);
        selection.cursor = QTextCursor(block);
        tints.append(selection);
    }
    setExtraSelections(tints + selections);
}

QString OptimizerEditor::textUnderCursor() const
{
    const QTextCursor cursor = textCursor();
    const QString blockText = cursor.block().text();
    const int position = cursor.position() - cursor.block().position();
    int start = position;
    while (start > 0 && isCompletionWordChar(blockText[start - 1]))
    {
        --start;
    }
    int end = position;
    while (end < blockText.size() && isCompletionWordChar(blockText[end]))
    {
        ++end;
    }
    return blockText.mid(start, end - start);
}

void OptimizerEditor::insertCompletion(const QString &completion)
{
    QTextCursor cursor = textCursor();
    const int prefixLength = m_completer->completionPrefix().length();
    cursor.setPosition(cursor.position() - prefixLength);
    cursor.setPosition(cursor.position() + prefixLength, QTextCursor::KeepAnchor);
    cursor.insertText(completion);
    setTextCursor(cursor);
}

void OptimizerEditor::keyPressEvent(QKeyEvent *event)
{
    // Same completion behaviour as the Editor tab.
    if (m_completer && m_completer->popup()->isVisible())
    {
        switch (event->key())
        {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Escape:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
            event->ignore();
            return;
        default:
            break;
        }
    }

    KitesEditor::keyPressEvent(event);

    if (!m_completer || isReadOnly())
    {
        return;
    }
    const QString prefix = textUnderCursor();
    const QStringList matches =
        prefix.isEmpty() ? QStringList() : LanguageService::completions(prefix, toPlainText());
    if (matches.isEmpty())
    {
        m_completer->popup()->hide();
        return;
    }
    auto *model = qobject_cast<QStringListModel *>(m_completer->model());
    model->setStringList(matches);
    m_completer->setCompletionPrefix(prefix);
    m_completer->popup()->setCurrentIndex(m_completer->completionModel()->index(0, 0));

    QRect area = cursorRect();
    area.setWidth(m_completer->popup()->sizeHintForColumn(0) +
                  m_completer->popup()->verticalScrollBar()->sizeHint().width());
    m_completer->complete(area);
}
} // namespace Kites
