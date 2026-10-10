#include <Heimdall/SourceSnapshot.hpp>

#include <gtest/gtest.h>

#include <future>
#include <string>
#include <vector>

namespace
{
    TEST(SourceSnapshotSpec, EditingOneByteSharesTheOtherPiecesAndKeepsOldViewsAlive)
    {
        const std::string source(20000, 'a');
        auto              before = heimdall::SourceSnapshot::Create(source);
        ASSERT_TRUE(before);
        const auto first = before->Chunk(0).data();

        auto after = before->Edit(10000, 1, "b");

        ASSERT_TRUE(after);
        EXPECT_EQ(after->Chunk(0).data(), first);
        EXPECT_EQ(after->CopiedBytes(), 1u);
        EXPECT_EQ(before->Read(9999, 3), "aaa");
        EXPECT_EQ(after->Read(9999, 3), "aba");
        EXPECT_EQ(*before->Materialize(), source);
        auto expected   = source;
        expected[10000] = 'b';
        EXPECT_EQ(*after->Materialize(), expected);
    }

    TEST(SourceSnapshotSpec, RejectsInvalidRangesAndHandlesDeletionToEmpty)
    {
        auto source = heimdall::SourceSnapshot::Create("abc");
        ASSERT_TRUE(source);

        EXPECT_FALSE(source->Edit(4, 0, "x"));
        EXPECT_FALSE(source->Edit(2, 2, "x"));
        auto empty = source->Edit(0, 3, "");

        ASSERT_TRUE(empty);
        EXPECT_EQ(empty->Size(), 0u);
        EXPECT_TRUE(empty->Materialize()->empty());
        auto inserted = empty->Edit(0, 0, "restored");
        ASSERT_TRUE(inserted);
        EXPECT_EQ(*inserted->Materialize(), "restored");
    }

    TEST(SourceSnapshotSpec, ConcurrentMaterializationPublishesOneBuffer)
    {
        auto source = heimdall::SourceSnapshot::Create(std::string(20000, 'a'));
        ASSERT_TRUE(source);
        std::vector<std::future<std::shared_ptr<const std::string>>> readers;

        for (int i = 0; i < 12; ++i)
        {
            readers.emplace_back(
                std::async(std::launch::async, [&] { return source->Materialize(); }));
        }
        auto expected = readers.front().get();

        for (std::size_t i = 1; i < readers.size(); ++i)
        {
            EXPECT_EQ(readers[i].get(), expected);
        }
    }
} // namespace

TEST(SourceSnapshotSpec, LongTypingSessionsBoundPieceMetadataWithoutChangingOldSnapshots)
{
    auto        source    = *heimdall::SourceSnapshot::Create("int value = 1;");
    const auto  before    = source;
    bool        compacted = false;
    std::string expected  = "int value = 1;";
    for (std::size_t i = 0; i < 1000; ++i)
    {
        source = *source.Edit(4, 0, "a");
        expected.insert(4, "a");
        compacted |= source.CopiedBytes() > 1;
    }

    EXPECT_TRUE(compacted);
    EXPECT_LT(source.ChunkCount(), 70u);
    EXPECT_EQ(*source.Materialize(), expected);
    EXPECT_EQ(*before.Materialize(), "int value = 1;");
}
