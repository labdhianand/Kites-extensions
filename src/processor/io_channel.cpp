#include "processor/io_channel.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <iostream>

namespace Kites
{
namespace
{
bool isSpace(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

// Position just after the first word in `text`, or npos if there is no complete word yet. A
// complete word is one followed by white space, because more digits could still be typed.
size_t endOfFirstToken(const std::string &text, bool inputEnded)
{
    size_t start = 0;
    while (start < text.size() && isSpace(text[start]))
    {
        ++start;
    }
    if (start == text.size())
    {
        return std::string::npos;
    }
    size_t end = start;
    while (end < text.size() && !isSpace(text[end]))
    {
        ++end;
    }
    if (end == text.size() && !inputEnded)
    {
        return std::string::npos;
    }
    return end;
}
} // namespace

IoChannel::IoChannel(QObject *parent) : QObject(parent)
{
}

// ---- Output ---------------------------------------------------------------------------------

void IoChannel::write(const std::string &text)
{
    if (text.empty())
    {
        return;
    }
    bool echo;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        output_ += text;
        echo = stdio_;
    }
    if (echo)
    {
        std::cout << text << std::flush;
    }
    emit outputWritten(QString::fromStdString(text));
}

void IoChannel::retractOutput(size_t count)
{
    size_t removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        removed = std::min(count, output_.size());
        output_.erase(output_.size() - removed);
    }
    if (removed > 0)
    {
        emit outputRetracted(static_cast<int>(removed));
    }
}

void IoChannel::replayOutput(const std::string &text)
{
    if (text.empty())
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        output_ += text;
    }
    emit outputWritten(QString::fromStdString(text));
}

std::string IoChannel::output() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return output_;
}

void IoChannel::clearOutput()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        output_.clear();
    }
    emit outputRetracted(-1); // -1: everything
}

// ---- Input ----------------------------------------------------------------------------------

void IoChannel::pushLine(const std::string &line)
{
    pushRaw(!line.empty() && line.back() == '\n' ? line : line + "\n");
}

void IoChannel::pushRaw(const std::string &text)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        input_ += text;
    }
    cv_.notify_all();
    emit inputChanged();
}

void IoChannel::unread(const std::string &text)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        input_.insert(0, text);
    }
    cv_.notify_all();
    emit inputChanged();
}

void IoChannel::discard(size_t count)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        input_.erase(0, std::min(count, input_.size()));
    }
    emit inputChanged();
}

bool IoChannel::hasToken() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return endOfFirstToken(input_, stdinEnded_) != std::string::npos;
}

bool IoChannel::hasChar() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !input_.empty();
}

bool IoChannel::hasLine() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return input_.find('\n') != std::string::npos || (stdinEnded_ && !input_.empty());
}

size_t IoChannel::pendingInput() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return input_.size();
}

void IoChannel::clearInput()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        input_.clear();
        stdinEnded_ = false;
    }
    emit inputChanged();
}

// ---- Waiting --------------------------------------------------------------------------------

bool IoChannel::pullFromStdin(std::unique_lock<std::mutex> &lock)
{
    lock.unlock();
    std::string line;
    const bool got = static_cast<bool>(std::getline(std::cin, line));
    lock.lock();
    if (!got)
    {
        stdinEnded_ = true;
        return false;
    }
    input_ += line + "\n";
    return true;
}

template <typename Ready>
ReadStatus IoChannel::waitFor(std::unique_lock<std::mutex> &lock, Ready ready,
                              const std::atomic<bool> *cancel)
{
    bool announced = false;
    ReadStatus result = ReadStatus::Ok;
    while (!ready())
    {
        if (cancel && cancel->load())
        {
            result = ReadStatus::Cancelled;
            break;
        }
        if (stdio_)
        {
            if (stdinEnded_ || !pullFromStdin(lock))
            {
                // The terminal has no more input. Whatever is left is all there will be.
                if (ready())
                {
                    break;
                }
                result = ReadStatus::Eof;
                break;
            }
            continue;
        }
        if (!blocking_)
        {
            result = ReadStatus::WouldWait;
            break;
        }
        if (!announced)
        {
            announced = true;
            lock.unlock();
            emit inputRequested(true);
            lock.lock();
            continue; // the input may have arrived while the lock was off
        }
        cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (announced)
    {
        lock.unlock();
        emit inputRequested(false);
        lock.lock();
    }
    return result;
}

ReadStatus IoChannel::readToken(std::string &token, std::string &consumed,
                                const std::atomic<bool> *cancel)
{
    std::unique_lock<std::mutex> lock(mutex_);
    const ReadStatus status = waitFor(
        lock, [&] { return endOfFirstToken(input_, stdinEnded_) != std::string::npos; }, cancel);
    if (status != ReadStatus::Ok)
    {
        return status;
    }
    const size_t end = endOfFirstToken(input_, stdinEnded_);
    size_t start = 0;
    while (isSpace(input_[start]))
    {
        ++start;
    }
    token = input_.substr(start, end - start);
    consumed = input_.substr(0, end);
    input_.erase(0, end);
    lock.unlock();
    emit inputChanged();
    return ReadStatus::Ok;
}

bool IoChannel::waitForChar()
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (input_.empty() && stdio_ && !stdinEnded_)
    {
        pullFromStdin(lock);
    }
    return !input_.empty();
}

bool IoChannel::takeChar(char &out)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (input_.empty())
        {
            return false;
        }
        out = input_.front();
        input_.erase(0, 1);
    }
    emit inputChanged();
    return true;
}

ReadStatus IoChannel::readChar(char &out, const std::atomic<bool> *cancel)
{
    std::unique_lock<std::mutex> lock(mutex_);
    const ReadStatus status = waitFor(lock, [&] { return !input_.empty(); }, cancel);
    if (status != ReadStatus::Ok)
    {
        return status;
    }
    out = input_.front();
    input_.erase(0, 1);
    lock.unlock();
    emit inputChanged();
    return ReadStatus::Ok;
}

ReadStatus IoChannel::readLine(std::string &out, size_t maxChars, const std::atomic<bool> *cancel)
{
    std::unique_lock<std::mutex> lock(mutex_);
    const ReadStatus status = waitFor(
        lock,
        [&]
        { return input_.find('\n') != std::string::npos || (stdinEnded_ && !input_.empty()); },
        cancel);
    if (status != ReadStatus::Ok)
    {
        return status;
    }
    size_t length = input_.find('\n');
    length = length == std::string::npos ? input_.size() : length + 1;
    length = std::min(length, maxChars);
    out = input_.substr(0, length);
    input_.erase(0, length);
    lock.unlock();
    emit inputChanged();
    return ReadStatus::Ok;
}

// ---- Settings -------------------------------------------------------------------------------

void IoChannel::setBlocking(bool blocking)
{
    std::lock_guard<std::mutex> lock(mutex_);
    blocking_ = blocking;
}

bool IoChannel::blocking() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return blocking_;
}

void IoChannel::useStdio(bool use)
{
    std::lock_guard<std::mutex> lock(mutex_);
    stdio_ = use;
}

bool IoChannel::usesStdio() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stdio_;
}

void IoChannel::wakeReaders()
{
    cv_.notify_all();
}

void IoChannel::reset()
{
    clearOutput();
    clearInput();
}

} // namespace Kites
