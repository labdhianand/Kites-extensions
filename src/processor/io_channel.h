/**
 * @file io_channel.h
 * @brief The text channel between a running program and whoever is watching it.
 *
 * Programs reach it through ecall (see syscall_handler.h) and through the memory-mapped console
 * (see mmio_devices.h). The GUI console, the CLI's stdin/stdout and the tests are all just
 * different users of the same channel, so every core behaves the same everywhere.
 *
 * Output is kept in a buffer as well as being announced, so that undo can take back what a step
 * printed, and redo can print it again without running the program a second time. Input works the
 * same way in reverse: what a step consumed is remembered by the step, and undo puts it back.
 */
#pragma once

#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>

namespace Kites
{
/// Something a step did to the outside world, remembered so undo and redo can respect it.
struct IoEvent
{
    enum class Kind
    {
        Output, ///< `text` was written to the console
        Input,  ///< `text` was taken from the input
    };

    Kind kind{Kind::Output};
    std::string text;

    bool operator==(const IoEvent &other) const = default;
};

/// What a read found.
enum class ReadStatus
{
    Ok,
    Eof,       ///< the input has ended (only with stdin as the input)
    WouldWait, ///< nothing to read yet, and this channel is not allowed to wait
    Cancelled, ///< the machine was stopped while waiting
};

class IoChannel : public QObject
{
    Q_OBJECT
  public:
    explicit IoChannel(QObject *parent = nullptr);

    // ---- Output -------------------------------------------------------------------------------

    /// Appends text to the output.
    void write(const std::string &text);

    /// Takes back the last `count` bytes of output (undo of a step that printed them).
    void retractOutput(size_t count);

    /// Puts text back that was taken back (redo). Unlike `write` it never goes to the terminal
    /// again, which already showed it once and cannot take it back.
    void replayOutput(const std::string &text);

    /// Everything the program has printed (and not had undone).
    std::string output() const;
    void clearOutput();

    // ---- Input --------------------------------------------------------------------------------

    /// Adds text typed by the user. A newline is appended if it does not end with one, because
    /// the user pressed Enter.
    void pushLine(const std::string &line);

    /// Adds text exactly as given (for tests and for putting back what undo gave back).
    void pushRaw(const std::string &text);

    /// Puts text back at the front of the input (undo of a step that consumed it).
    void unread(const std::string &text);

    /// Drops text from the front of the input (redo of a step that consumed it).
    void discard(size_t count);

    /// True if a read of a word could finish right now.
    bool hasToken() const;

    /// True if a read of a character could finish right now.
    bool hasChar() const;

    /// True if a read of a line could finish right now.
    bool hasLine() const;

    /// Characters waiting to be read.
    size_t pendingInput() const;

    void clearInput();

    // ---- Reading, used by the syscalls --------------------------------------------------------

    /// Takes the next whitespace-delimited word. `consumed` is everything removed from the input
    /// (the word and the white space before it), which is what undo has to put back. `cancel` is
    /// polled while waiting.
    ReadStatus readToken(std::string &token, std::string &consumed, const std::atomic<bool> *cancel);

    /// For the memory-mapped keyboard: true if a character is waiting. With the terminal as input
    /// this waits for a line to be typed; otherwise it returns at once.
    bool waitForChar();

    /// Takes one character if there is one, without waiting.
    bool takeChar(char &out);

    /// Takes one character.
    ReadStatus readChar(char &out, const std::atomic<bool> *cancel);

    /// Takes the rest of the current line including the newline, but at most `maxChars`.
    ReadStatus readLine(std::string &out, size_t maxChars, const std::atomic<bool> *cancel);

    /// Whether a read may wait for the user. The GUI's Step button turns this off, so a program
    /// that asks for input does not freeze the window; Run leaves it on.
    void setBlocking(bool blocking);
    bool blocking() const;

    /// Makes output go to the terminal and, when the buffer runs dry, input come from stdin.
    void useStdio(bool use);
    bool usesStdio() const;

    /// Wakes up readers so they notice the machine was stopped.
    void wakeReaders();

    /// Empties both directions (Reset).
    void reset();

  signals:
    void outputWritten(const QString &text);
    void outputRetracted(int count);
    void inputRequested(bool waiting);
    void inputChanged();

  private:
    // Waits until `ready()` holds on the pending input. Called with the lock held.
    template <typename Ready>
    ReadStatus waitFor(std::unique_lock<std::mutex> &lock, Ready ready,
                       const std::atomic<bool> *cancel);

    // With stdin as the input, tries to get one more line from the terminal.
    bool pullFromStdin(std::unique_lock<std::mutex> &lock);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string output_;
    std::string input_;
    bool blocking_{true};
    bool stdio_{false};
    bool stdinEnded_{false};
};

} // namespace Kites
