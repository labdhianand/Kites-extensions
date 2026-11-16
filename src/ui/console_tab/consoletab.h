#pragma once
#include "processor/io_channel.h"
#include "ui/common/kitestab.h"

#include <QString>
#include <QVector>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace Kites
{
/**
 * @brief The Console tab: what the running program prints, and a line to type its input.
 *
 * It shows an IoChannel, so it works with whichever processor model is selected. The channel
 * lives on the processor's thread; everything arrives here as queued signals.
 *
 * Undo takes back what the undone step printed, so the output shown here always matches the
 * machine's state. What was typed stays on screen (marked as input) because it cannot be
 * un-typed; the program gets it again when it is run forward.
 */
class ConsoleTab : public KitesTab
{
    Q_OBJECT
  public:
    explicit ConsoleTab(IoChannel *channel, QWidget *parent = nullptr);

    /// Switches to another channel (the one the new processor uses).
    void setChannel(IoChannel *channel);

  signals:
    /// The program is waiting for input; the window brings this tab to the front.
    void inputWanted();

  private slots:
    void appendOutput(const QString &text);
    void retractOutput(int count);
    void waitingForInput(bool waiting);
    void submit();
    void clear();

  private:
    struct Segment
    {
        QString text;
        bool isInput;
    };

    void append(const QString &text, bool isInput);
    void rebuild();
    void connectChannel();

    IoChannel *m_channel{nullptr};
    QVector<Segment> m_segments;
    QPlainTextEdit *m_view{nullptr};
    QLineEdit *m_input{nullptr};
    QPushButton *m_send{nullptr};
    QLabel *m_status{nullptr};
};
} // namespace Kites
