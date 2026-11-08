#pragma once
#include "ui/common/kites_editor.h"

#include <QColor>
#include <vector>

class QCompleter;

namespace Kites
{
class SyntaxHighlighter;

/**
 * @brief A code view for the Optimizer tab: syntax highlighting, line numbers, and a soft tint
 * behind the lines that differ between the original and the suggestion.
 *
 * The original side is editable and types like the Editor tab (same font, tab width, current-line
 * highlight and instruction/label completion); the suggested side is read-only.
 */
class OptimizerEditor : public KitesEditor
{
    Q_OBJECT
  public:
    enum class Tint
    {
        Removed, ///< lines only in the original
        Added    ///< lines only in the suggestion
    };

    OptimizerEditor(Tint tint, bool editable, QWidget *parent = nullptr);

    /// Shows `text` and tints the given 1-based lines.
    void showProgram(const QString &text, const std::vector<int> &changedLines);
    /// Tints the given 1-based lines of the text already shown.
    void setTintedLines(const std::vector<int> &changedLines);

  protected:
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;

  private slots:
    void applyTints();
    void insertCompletion(const QString &completion);

  private:
    QString textUnderCursor() const;

    Tint m_tint;
    std::vector<int> m_changedLines;
    SyntaxHighlighter *m_syntaxHighlighter{nullptr};
    QCompleter *m_completer{nullptr};
};
} // namespace Kites
