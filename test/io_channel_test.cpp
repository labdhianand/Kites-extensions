#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "processor/io_channel.h"

using namespace Kites;

TEST(IoChannel, OutputIsBufferedAndCanBeRetractedAndReplayed)
{
    IoChannel io;
    io.write("hello ");
    io.write("world");
    io.retractOutput(5);
    EXPECT_EQ(io.output(), "hello ");
    io.replayOutput("world");
    EXPECT_EQ(io.output(), "hello world");
    io.retractOutput(1000); // more than there is
    EXPECT_EQ(io.output(), "");
}

TEST(IoChannel, TokenNeedsTheWhiteSpaceThatEndsIt)
{
    IoChannel io;
    io.setBlocking(false);
    std::string token, consumed;
    io.pushRaw("12");
    EXPECT_EQ(io.readToken(token, consumed, nullptr), ReadStatus::WouldWait); // more digits may come
    io.pushRaw("3 4\n");
    ASSERT_EQ(io.readToken(token, consumed, nullptr), ReadStatus::Ok);
    EXPECT_EQ(token, "123");
    EXPECT_EQ(consumed, "123");
    ASSERT_EQ(io.readToken(token, consumed, nullptr), ReadStatus::Ok);
    EXPECT_EQ(token, "4");
    EXPECT_EQ(consumed, " 4"); // the white space before the word goes with it
}

TEST(IoChannel, UnreadPutsConsumedTextBackInFront)
{
    IoChannel io;
    io.pushLine("7 8");
    std::string token, consumed;
    ASSERT_EQ(io.readToken(token, consumed, nullptr), ReadStatus::Ok);
    io.unread(consumed);
    ASSERT_EQ(io.readToken(token, consumed, nullptr), ReadStatus::Ok);
    EXPECT_EQ(token, "7");
}

TEST(IoChannel, LineReadStopsAtTheNewlineAndTheLimit)
{
    IoChannel io;
    io.pushRaw("abcdef\nxyz\n");
    std::string line;
    ASSERT_EQ(io.readLine(line, 4, nullptr), ReadStatus::Ok);
    EXPECT_EQ(line, "abcd");
    ASSERT_EQ(io.readLine(line, 100, nullptr), ReadStatus::Ok);
    EXPECT_EQ(line, "ef\n");
}

TEST(IoChannel, BlockedReadWakesWhenInputArrives)
{
    IoChannel io;
    std::thread typist(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            io.pushLine("5");
        });
    std::string token, consumed;
    EXPECT_EQ(io.readToken(token, consumed, nullptr), ReadStatus::Ok);
    EXPECT_EQ(token, "5");
    typist.join();
}

TEST(IoChannel, BlockedReadCanBeCancelled)
{
    IoChannel io;
    std::atomic<bool> cancel{false};
    std::thread stopper(
        [&]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            cancel = true;
            io.wakeReaders();
        });
    char c;
    EXPECT_EQ(io.readChar(c, &cancel), ReadStatus::Cancelled);
    stopper.join();
}
