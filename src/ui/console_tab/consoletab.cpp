#include "consoletab.h"

#include <algorithm>

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>

namespace Kites
{
ConsoleTab::ConsoleTab(IoChannel *channel, QWidget *parent) : KitesTab(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    m_view = new QPlainTextEdit(this);
    m_view->setReadOnly(true);
    m_view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_view->setPlaceholderText(
        "Program output appears here. Programs print with ecall (a7 = 1, 2, 3, 4, 11, 64) or by "
        "writing characters to the memory-mapped display at 0x2000000C.");
    layout->addWidget(m_view, 1);

    auto *row = new QHBoxLayout();
    row->setSpacing(6);
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText("Type input for the program and press Enter");
    m_input->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_send = new QPushButton("Send", this);
    auto *clearButton = new QPushButton("Clear", this);
    row->addWidget(m_input, 1);
    row->addWidget(m_send);
    row->addWidget(clearButton);
    layout->addLayout(row);

    m_status = new QLabel(this);
    layout->addWidget(m_status);

    connect(m_input, &QLineEdit::returnPressed, this, &ConsoleTab::submit);
    connect(m_send, &QPushButton::clicked, this, &ConsoleTab::submit);
    connect(clearButton, &QPushButton::clicked, this, &ConsoleTab::clear);

    setChannel(channel);
}

void ConsoleTab::setChannel(IoChannel *channel)
{
    if (m_channel)
    {
        disconnect(m_channel, nullptr, this, nullptr);
    }
    m_channel = channel;
    connectChannel();
    // Show what the new channel already holds (a fresh one holds nothing).
    m_segments.clear();
    if (m_channel)
    {
        const QString existing = QString::fromStdString(m_channel->output());
        if (!existing.isEmpty())
        {
            m_segments.push_back({existing, false});
        }
    }
    rebuild();
    waitingForInput(false);
}

void ConsoleTab::connectChannel()
{
    if (!m_channel)
    {
        return;
    }
    // The program runs on another thread, so these always arrive queued.
    connect(m_channel, &IoChannel::outputWritten, this, &ConsoleTab::appendOutput,
            Qt::QueuedConnection);
    connect(m_channel, &IoChannel::outputRetracted, this, &ConsoleTab::retractOutput,
            Qt::QueuedConnection);
    connect(m_channel, &IoChannel::inputRequested, this, &ConsoleTab::waitingForInput,
            Qt::QueuedConnection);
}

void ConsoleTab::append(const QString &text, bool isInput)
{
    if (text.isEmpty())
    {
        return;
    }
    if (!m_segments.isEmpty() && m_segments.back().isInput == isInput)
    {
        m_segments.back().text += text;
    }
    else
    {
        m_segments.push_back({text, isInput});
    }

    QTextCharFormat format;
    if (isInput)
    {
        format.setForeground(palette().color(QPalette::Link));
    }
    QTextCursor cursor(m_view->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text, format);
    m_view->moveCursor(QTextCursor::End);
}

void ConsoleTab::appendOutput(const QString &text)
{
    append(text, false);
}

void ConsoleTab::retractOutput(int count)
{
    if (count < 0)
    {
        m_segments.clear(); // everything: the machine was reset
        rebuild();
        return;
    }
    // Take `count` characters off the end of the program's output, leaving what was typed.
    for (int i = m_segments.size() - 1; i >= 0 && count > 0; --i)
    {
        if (m_segments[i].isInput)
        {
            continue;
        }
        const int removed = qMin(count, static_cast<int>(m_segments[i].text.size()));
        m_segments[i].text.chop(removed);
        count -= removed;
    }
    m_segments.erase(std::remove_if(m_segments.begin(), m_segments.end(),
                                    [](const Segment &s) { return s.text.isEmpty(); }),
                     m_segments.end());
    rebuild();
}

void ConsoleTab::rebuild()
{
    m_view->clear();
    const QVector<Segment> segments = m_segments;
    m_segments.clear();
    for (const Segment &segment : segments)
    {
        append(segment.text, segment.isInput);
    }
}

void ConsoleTab::waitingForInput(bool waiting)
{
    m_status->setText(waiting ? "The program is waiting for input." : QString());
    if (waiting)
    {
        m_input->setFocus();
        emit inputWanted();
    }
}

void ConsoleTab::submit()
{
    if (!m_channel)
    {
        return;
    }
    const QString line = m_input->text();
    m_input->clear();
    append(line + "\n", true);
    m_channel->pushLine(line.toStdString());
}

void ConsoleTab::clear()
{
    m_segments.clear();
    m_view->clear();
    if (m_channel)
    {
        m_channel->clearOutput();
    }
}
} // namespace Kites
