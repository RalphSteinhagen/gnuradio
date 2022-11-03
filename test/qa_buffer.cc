#include <gtest/gtest.h>

#include <algorithm>
#include <numeric>
#include <ranges>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <gnuradio/buffer.hpp>
#include <gnuradio/buffer_host.hpp>
#include <gnuradio/buffer_skeleton.hpp>
#include <gnuradio/sequence.hpp>
#include <gnuradio/wait_strategy.hpp>

using buffer_typelist = testing::Types< //
    gr::test::buffer_skeleton<int32_t>,
    gr::buffer_host<int32_t>>;
template <class>
struct Buffers : testing::Test {
};
TYPED_TEST_SUITE(Buffers, buffer_typelist);

TYPED_TEST(Buffers, BasicConcepts)
{
    using namespace gr;
    Buffer auto buffer = TypeParam(1024);
    // N.B. GE because some buffers need to intrinsically
    // allocate more to meet e.g. page-size requirements
    EXPECT_GE(buffer.size(), 1024);

    // compile-time interface tests
    BufferReader auto reader = buffer.newReaderInstance(); // tests matching read concept
    BufferWriter auto writer = buffer.newWriterInstance(); // tests matching write concept

    // runtime interface tests
    EXPECT_EQ(reader.available(), 0);
    EXPECT_NO_THROW(EXPECT_EQ(reader.get(0).size(), 0));
    EXPECT_NO_THROW(EXPECT_TRUE(reader.consume(0)));

    EXPECT_GE(writer.available(), buffer.size());
    EXPECT_NO_THROW(writer.publish([](std::span<int32_t>&) {}, 0));
    EXPECT_NO_THROW(EXPECT_TRUE(writer.tryPublish([](std::span<int32_t>&) {}, 0)));
}

TEST(Buffer, Sequence)
{
    using namespace gr;
    EXPECT_EQ(alignof(Sequence), kCacheLine);
    EXPECT_EQ(-1L, kInitialCursorValue);
    EXPECT_NO_THROW(Sequence());
    EXPECT_NO_THROW(Sequence(2));

    auto s1 = Sequence();
    EXPECT_EQ(s1.value(), kInitialCursorValue);

    const auto s2 = Sequence(2);
    EXPECT_EQ(s2.value(), 2);

    EXPECT_NO_THROW(s1.setValue(3));
    EXPECT_EQ(s1.value(), 3);

    EXPECT_NO_THROW(EXPECT_TRUE(s1.compareAndSet(3, 4)));
    EXPECT_EQ(s1.value(), 4);
    EXPECT_NO_THROW(EXPECT_FALSE(s1.compareAndSet(3, 5)));
    EXPECT_EQ(s1.value(), 4);

    EXPECT_EQ(s1.incrementAndGet(), 5);
    EXPECT_EQ(s1.value(), 5);
    EXPECT_EQ(s1.addAndGet(2), 7);
    EXPECT_EQ(s1.value(), 7);

    std::shared_ptr<std::vector<std::shared_ptr<Sequence>>> sequences{
        std::make_shared<std::vector<std::shared_ptr<Sequence>>>()
    };
    EXPECT_EQ(detail::getMinimumSequence(*sequences),
              std::numeric_limits<std::int64_t>::max());
    EXPECT_EQ(detail::getMinimumSequence(*sequences, 2), 2);
    sequences->emplace_back(std::make_shared<Sequence>(4));
    EXPECT_EQ(detail::getMinimumSequence(*sequences), 4);
    EXPECT_EQ(detail::getMinimumSequence(*sequences, 5), 4);
    EXPECT_EQ(detail::getMinimumSequence(*sequences, 2), 2);

    auto cursor = std::make_shared<Sequence>(10);
    auto s3 = std::make_shared<Sequence>(1);
    EXPECT_EQ(sequences->size(), 1);
    EXPECT_EQ(detail::getMinimumSequence(*sequences), 4);
    EXPECT_NO_THROW(detail::addSequences(sequences, *cursor, { s3 }));
    EXPECT_EQ(sequences->size(), 2);
    // newly added sequences are set automatically to the cursor/write position
    EXPECT_EQ(s3->value(), 10);
    EXPECT_EQ(detail::getMinimumSequence(*sequences), 4);

    EXPECT_NO_THROW(detail::removeSequence(sequences, cursor));
    EXPECT_EQ(sequences->size(), 2);
    EXPECT_NO_THROW(detail::removeSequence(sequences, s3));
    EXPECT_EQ(sequences->size(), 1);

    std::stringstream ss;
    EXPECT_EQ(ss.str().size(), 0);
    EXPECT_NO_THROW(ss << fmt::format("{}", *s3));
    EXPECT_NE(ss.str().size(), 0);
}

template <gr::WaitStrategy auto wait = gr::NoWaitStrategy()>
struct TestStruct {
    [[nodiscard]] constexpr bool test() const noexcept { return true; }
};

TEST(Buffer, WaitStrategies)
{
    using namespace gr;

    EXPECT_TRUE(isWaitStrategy<BlockingWaitStrategy>);
    EXPECT_TRUE(isWaitStrategy<BusySpinWaitStrategy>);
    EXPECT_TRUE(isWaitStrategy<SleepingWaitStrategy>);
    EXPECT_TRUE(isWaitStrategy<SleepingWaitStrategy>);
    EXPECT_TRUE(isWaitStrategy<SpinWaitWaitStrategy>);
    EXPECT_TRUE(isWaitStrategy<TimeoutBlockingWaitStrategy>);
    EXPECT_TRUE(isWaitStrategy<YieldingWaitStrategy>);
    EXPECT_TRUE(not isWaitStrategy<int>);

    EXPECT_TRUE(WaitStrategy<BlockingWaitStrategy>);
    EXPECT_TRUE(WaitStrategy<BusySpinWaitStrategy>);
    EXPECT_TRUE(WaitStrategy<SleepingWaitStrategy>);
    EXPECT_TRUE(WaitStrategy<SleepingWaitStrategy>);
    EXPECT_TRUE(WaitStrategy<SpinWaitWaitStrategy>);
    EXPECT_TRUE(WaitStrategy<TimeoutBlockingWaitStrategy>);
    EXPECT_TRUE(WaitStrategy<YieldingWaitStrategy>);
    EXPECT_TRUE(not WaitStrategy<int>);

    TestStruct a;
    EXPECT_TRUE(a.test());
}

// disable clang format to improve readability -> TODO: re-evaluate rules
// clang-format off
TEST(Buffer, UserApiExamples)
{
    using namespace gr;
    Buffer auto buffer = buffer_host<int32_t>(1024);

    BufferWriter auto writer = buffer.newWriterInstance();
    { // source only write example
        BufferReader auto localReader = buffer.newReaderInstance();
        EXPECT_EQ(localReader.available(), 0);

        auto lambda = [](auto w) { // test writer generating consecutive samples
            static int offset = 1;
            std::iota(w.begin(), w.end(), offset);
            offset += w.size();
        };

        EXPECT_GE(writer.available(), buffer.size());
        writer.publish(lambda, 10);
        EXPECT_EQ(writer.available(), buffer.size() - 10);
        EXPECT_EQ(localReader.available(), 10);
        EXPECT_EQ(buffer.nReaders(), 1); // N.B. buffer_host<..> specific
    }
    EXPECT_EQ(buffer.nReaders(), 0); // reader not in scope release atomic reader index

    BufferReader auto reader = buffer.newReaderInstance();
    // reader does not know about previous submitted data as it joined only after
    // data has been written <-> needed for thread-safe joining of readers while writing
    EXPECT_EQ(reader.available(), 0);
    // populate with some more data
    for (int i = 0; i < 3; i++) {
        const auto demoWriter = [](auto w) {
            static int offset = 1;
            std::iota(w.begin(), w.end(), offset);
            offset += w.size();
        };
        writer.publish(demoWriter, 5); // writer/publish five samples
        EXPECT_EQ(reader.available(), (i + 1) * 5);
    }

    // N.B. here using a simple read-only (sink) example:
    for (int i = 0; reader.available() != 0; i++) {
        std::span<const int32_t> fixedLength = reader.get(3); // explicitly typed for illustration
        auto available = reader.get();
        fmt::print("iteration {} - fixed-size data[{:2}]: [{}]\n", i, fixedLength.size(), fmt::join(fixedLength, ", "));
        fmt::print("iteration {} - full-size  data[{:2}]: [{}]\n", i, available.size(), fmt::join(available, ", "));

        // consume data -> allows corresponding buffer to be overwritten by writer
        // if there are no other reader claiming that buffer segment
        if (reader.consume(fixedLength.size())) {
            // for info-only - since available() can change in parallel
            // N.B. lock-free buffer and other writer may add while processing
            fmt::print("iteration {} - consumed {} elements - still available: {}\n", i, fixedLength.size(), reader.available());
        } else {
            throw std::runtime_error(fmt::format("could not consume data"));
        }
    }
}
// clang-format on

// clang-format off
using Allocator = std::pmr::polymorphic_allocator<int32_t>;
class BufferTestFixture :public ::testing::TestWithParam<Allocator> { };

Allocator allocators[] = { Allocator(gr::DoubleMappedAllocator::defaultAllocator()), Allocator(), Allocator(gr::CStyleAllocator::defaultAllocator())};
//"double-mapped allocator", "C++ pmt allocator", "C-style allocator"

TEST_P(BufferTestFixture, BufferHostImplementation)
{
    using namespace gr;
    Buffer auto buffer = buffer_host<int32_t>(1024, GetParam());
    EXPECT_GE(buffer.size(), 1024);

    BufferWriter auto writer = buffer.newWriterInstance();
    BufferReader auto reader = buffer.newReaderInstance();

    int offset = 1;
    auto lambda = [&offset](auto w) {
        std::iota(w.begin(), w.end(), offset);
        offset += w.size();
    };

    EXPECT_EQ(reader.available(), 0);
    EXPECT_EQ(reader.get().size(), 0);
    EXPECT_EQ(reader.get(1).size(), 0);
    EXPECT_EQ(writer.available(), buffer.size());
    EXPECT_FALSE(reader.consume(1)); // false: no data available yet
    EXPECT_NO_THROW(writer.publish(lambda, buffer.size())); // fully fill buffer

    EXPECT_EQ(writer.available(), 0);
    EXPECT_EQ(reader.available(), buffer.size());
    EXPECT_EQ(reader.get().size(), buffer.size());
    EXPECT_EQ(reader.get(1).size(), 1);

    // full buffer: fill buffer need to fail/return 'false'
    EXPECT_FALSE(writer.tryPublish(lambda, buffer.size()));

    EXPECT_TRUE(reader.consume(buffer.size()));
    EXPECT_EQ(reader.available(), 0);
    EXPECT_EQ(writer.available(), buffer.size());

    // test buffer wrap around twice
    int32_t counter = 1;
    for (const std::size_t blockSize: {1, 2, 3, 5, 7, 42}) {
        for (uint32_t i = 0; i < buffer.size(); i++) {
            EXPECT_TRUE(writer.tryPublish([&counter](auto& writable) {
                std::iota(writable.begin(), writable.end(), counter += writable.size());
            }, blockSize));
            auto readable = reader.get();
            EXPECT_EQ(readable.size(), blockSize);
            EXPECT_EQ(readable.front() , counter);
            EXPECT_EQ(readable.back(), counter + blockSize - 1);
            EXPECT_TRUE(reader.consume(blockSize));
        }
    }

    //TODO: create tests for
    // BufferReader auto readerSmaller = buffer.newReaderInstance<uint8_t>();
    // BufferReader auto readerLarger = buffer.newReaderInstance<uint64_t>();
    // or via https://en.cppreference.com/w/cpp/container/span/as_bytes

}

INSTANTIATE_TEST_SUITE_P(AllocatorTests, BufferTestFixture,
    ::testing::Values(Allocator(gr::DoubleMappedAllocator::defaultAllocator()), Allocator(), Allocator(gr::CStyleAllocator::defaultAllocator())));

// clang-format on
