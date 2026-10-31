#include <trantor/utils/LogStream.h>
#include <gtest/gtest.h>
#include <string>

TEST(FmtTest, truncatedOutputUsesStoredLength)
{
    trantor::Fmt formatted("%1000d", 1);
    trantor::LogStream stream;
    stream << formatted;

    EXPECT_EQ(47, formatted.length());
    EXPECT_EQ(static_cast<size_t>(formatted.length()), stream.bufferLength());
    EXPECT_EQ(std::string(47, ' '),
              std::string(stream.bufferData(), stream.bufferLength()));
}

int main(int argc, char **argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
