#ifndef GNURADIO_BUFFER_HOST_HPP
#define GNURADIO_BUFFER_HOST_HPP

#include <memory_resource>
#include <algorithm>
#include <cassert> // to assert if compiled for debugging
#include <functional>
#include <numeric>
#include <ranges>
#include <span>

#include <fmt/format.h>

// header for creating/opening or POSIX shared memory objects
#include <errno.h>
#include <fcntl.h>
#if defined __has_include
#if __has_include(<sys/mman.h>) && __has_include(<sys/stat.h>) && __has_include(<unistd.h>)
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
namespace gr {
static constexpr bool has_posix_mmap_interface = true;
}
#elif
namespace gr {
static constexpr bool has_posix_mmap_interface = false;
}
#endif
#elif // #if defined __has_include -- required for portability
namespace gr {
static constexpr bool has_posix_mmap_interface = false;
}
#endif

#include <gnuradio/buffer.hpp>
#include <gnuradio/claim_strategy.hpp>
#include <gnuradio/sequence.hpp>
#include <gnuradio/wait_strategy.hpp>


namespace gr {

// clang-format off
class CStyleAllocator : public std::pmr::memory_resource {
    [[nodiscard]] void* do_allocate(std::size_t bytes, std::size_t /*alignment*/) override { return malloc(bytes); }
    void  do_deallocate(void* p, size_t /*bytes*/, size_t /*alignment*/) override { return free(p); }
    bool  do_is_equal(const memory_resource& other) const noexcept override { return this == &other; }

public:
    static inline CStyleAllocator* defaultAllocator()
    {
        static CStyleAllocator instance = CStyleAllocator();
        return &instance;
    }
};

class DoubleMappedAllocator : public std::pmr::memory_resource {
    [[nodiscard]] void* do_allocate(std::size_t size, std::size_t /*alignment*/) override {
        if (size % getpagesize() != 0) {
            throw std::runtime_error(fmt::format("incompatible buffer-byte-size: {} vs. page size: {}", size, getpagesize()));
        }

        static int _counter;
        const auto buffer_name = fmt::format("/DoubleMappedAllocator-{}-{}-{}", getpid(), size, _counter++);
        const auto memfd_create = [name = buffer_name.c_str()](unsigned int flags) -> int {
            return syscall(__NR_memfd_create, name, flags);
        };
        int shm_fd = memfd_create(0);
        if (shm_fd < 0) {
            throw std::runtime_error(fmt::format("{} - memfd_create error {}: {}",  buffer_name, errno, strerror(errno)));
        }

        if (ftruncate(shm_fd, (off_t)2 * size) == -1) {
            close(shm_fd);
            throw std::runtime_error(fmt::format("{} - ftruncate {}: {}",  buffer_name, errno, strerror(errno)));
        }

        void* first_copy = mmap(0, 2 * size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, (off_t)0);
        if (first_copy == MAP_FAILED) {
            close(shm_fd);
            throw std::runtime_error(fmt::format("{} - failed munmap for first half {}: {}",  buffer_name, errno, strerror(errno)));
        }

        // unmap the 2nd half
        if (munmap((char*)first_copy + size, size) == -1) {
            close(shm_fd);
            throw std::runtime_error(fmt::format("{} - failed munmap for second half {}: {}",  buffer_name, errno, strerror(errno)));
        }

        // map the first half into the now available hole where the
        void* second_copy = mmap((char*)first_copy + size, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, (off_t)0);
        if (second_copy == MAP_FAILED) {
            close(shm_fd);
            throw std::runtime_error(fmt::format("{} - failed mmap for second copy {}: {}",  buffer_name, errno, strerror(errno)));
        }

        close(shm_fd); // file-descriptor is no longer needed. The mapping is retained.
        return first_copy;
    }

    void  do_deallocate(void* p, std::size_t size, size_t alignment) override {
        if (munmap(p, 2 * size) == -1) {
            throw std::runtime_error(fmt::format("DoubleMappedAllocator::do_deallocate(void*, {}, {}) - munmap(..) failed", size, alignment));
        }
    }

    bool  do_is_equal(const memory_resource& other) const noexcept override { return this == &other; }

public:
    static inline DoubleMappedAllocator* defaultAllocator()
    {
        static DoubleMappedAllocator instance = DoubleMappedAllocator();
        return &instance;
    }
};



/**
 * @brief circular buffer implementation using double-mapped memory allocations
 * where the first SIZE-ed buffer is mirrored directly its end to mimic wrap-around
 * free bulk memory access. The buffer keeps a list of indices (using 'Sequence')
 * to keep track of which parts can be tread-safely read and/or written
 *
 *                          wrap-around point
 *                                 |
 *                                 v
 *  | buffer segment #1 (original) | buffer segment #2 (copy of #1) |
 *  0                            SIZE                            2*SIZE
 *                      writeIndex
 *                          v
 *  wrap-free write access  |<-  N_1 < SIZE   ->|
 *
 *  readIndex < writeIndex-N_2
 *      v
 *      |<- N_2 < SIZE ->|
 *
 * N.B N_AVAILABLE := (SIZE + writeIndex - readIndex ) % SIZE
 *
 * citation: <find appropriate first and educational reference>
 *
 * This implementation provides single- as well as multi-producer/consumer buffer
 * combinations for thread-safe CPU-to-CPU data transfer (optionally) using either
 * a) the POSIX mmaped(..)/munmapped(..) MMU interface, if available, and/or
 * b) the guaranteed portable standard C/C++ (de-)allocators as a fall-back.
 *
 * for more details see
 */
template <typename T, std::size_t SIZE = std::dynamic_extent, ProducerType producerType = ProducerType::Single, WaitStrategy WAIT_STRATEGY = SleepingWaitStrategy>
class buffer_host
{
    using Allocator         = std::pmr::polymorphic_allocator<T>;
    using BufferType        = buffer_host<T, SIZE, producerType, WAIT_STRATEGY>;
    using ClaimType         = detail::producer_type_v<SIZE, producerType, WAIT_STRATEGY>;
    using DependendsType    = std::shared_ptr<std::vector<std::shared_ptr<Sequence>>>;

    struct buffer_impl {
        alignas(kCacheLine) Allocator           _allocator{};
        alignas(kCacheLine) const bool          _is_mmap_allocated;
        alignas(kCacheLine) const std::size_t   _size;
        alignas(kCacheLine) T*                  _data; // TODO: check whether cleaner std::vector wouldn't be as fast or faster
        alignas(kCacheLine) Sequence            _cursor;
        alignas(kCacheLine) WAIT_STRATEGY       _waitStrategy = WAIT_STRATEGY();
        alignas(kCacheLine) ClaimType           _claimStrategy;
        // list of dependent reader indices
        alignas(kCacheLine) DependendsType      _readIndices{ std::make_shared<std::vector<std::shared_ptr<Sequence>>>() };

        buffer_impl() = delete;
        buffer_impl(const std::size_t min_size, Allocator allocator) : _allocator(allocator), _is_mmap_allocated(dynamic_cast<DoubleMappedAllocator *>(_allocator.resource())),
            _size(alignWithPageSize(min_size, _is_mmap_allocated)), _data(static_cast<T*>(_allocator.allocate(_size * sizeof(T)))), _claimStrategy(ClaimType(_cursor, _waitStrategy, _size)) {
            if constexpr (std::is_class_v<T>) {
                for (int i = 0; i < _size; i++) {
                    new (static_cast<T*>(_data) + i) T; // call class constructor to init dependent internal structures/state
                }
            } else {
                std::memset(_data, 0, 2 * _size * sizeof(T)); // optional init jut for cleaner testing
            }
        }
        ~buffer_impl() noexcept(false) {
            assert(_data != nullptr && "buffer_host has been already destroyed");
            if constexpr (std::is_class_v<T>) {
                for (int i = 0; i < _size; i++) {
                    _data[i].~T(); // call the stored class destructors.
                }
            }
            _allocator.deallocate(_data, _size);
            _data = nullptr;
        };

    private:
        static int roundUp(int numToRound, int multiple) {
            if (multiple == 0) {
                return numToRound;
            }
            const int remainder = numToRound % multiple;
            if (remainder == 0) {
                return numToRound;
            }
            return numToRound + multiple - remainder;
        }

        static std::size_t alignWithPageSize(const std::size_t min_size, bool _is_mmap_allocated) {
            return _is_mmap_allocated ? roundUp(2 * min_size * sizeof(T), getpagesize())/2 : min_size;
        }
    };

    template <typename U>
    class buffer_host_writer {
        using BufferType = std::shared_ptr<buffer_impl>;

        alignas(kCacheLine) BufferType          _buffer; // controls buffer life-cycle, the rest are cache optimisations
        alignas(kCacheLine) const bool          _is_mmap_allocated;
        alignas(kCacheLine) DependendsType&     _readIndices;
        alignas(kCacheLine) const std::size_t   _size;
        alignas(kCacheLine) T*                  _data;
        alignas(kCacheLine) ClaimType&          _claimStrategy;

    public:
        buffer_host_writer() = delete;
        buffer_host_writer(std::shared_ptr<buffer_impl> buffer) :
            _buffer(buffer), _is_mmap_allocated(_buffer->_is_mmap_allocated), _readIndices(buffer->_readIndices),
            _size(buffer->_size), _data(buffer->_data), _claimStrategy(buffer->_claimStrategy) { };

        template <std::invocable<std::span<U>&> Translator, typename... Args>
        constexpr void publish(Translator&& translator, std::size_t n_slots_to_claim = 1, Args&&... args) {
            if (n_slots_to_claim <= 0 || _readIndices->empty()) {
                return;
            }
            const auto sequence = _claimStrategy.next(*_readIndices, n_slots_to_claim);
            translateAndPublish(std::forward<Translator>(translator), n_slots_to_claim, sequence, std::forward<Args>(args)...);
        }; // blocks until elements are available

        template <std::invocable<std::span<U>&> Translator, typename... Args>
        constexpr bool tryPublish(Translator&& translator, std::size_t n_slots_to_claim = 1, Args&&... args) noexcept {
            if (n_slots_to_claim <= 0 || _readIndices->empty()) {
                return true;
            }
            try {
                const auto sequence = _claimStrategy.tryNext(*_readIndices, n_slots_to_claim);
                translateAndPublish(std::forward<Translator>(translator), n_slots_to_claim, sequence, std::forward<Args>(args)...);
                return true;
            } catch (const NoCapacityException &) {
                return false;
            }
        };

        [[nodiscard]] constexpr std::size_t available() const noexcept {
            return _claimStrategy.getRemainingCapacity(*_readIndices);
        }

        private:
        template <typename... TArgs, std::invocable<std::span<T>&, TArgs...> Translator>
        constexpr void translateAndPublish(Translator&& translator, const std::size_t n_slots_to_claim, const std::int64_t publishSequence, const TArgs&... args) noexcept {
            try {
                const auto index = (publishSequence + _size - n_slots_to_claim) % _size;
                std::span<T> writableData = { &_data[index], n_slots_to_claim };
                std::invoke(std::forward<Translator>(translator), std::forward<std::span<T>&>(writableData), args...);
                if (!_is_mmap_allocated) {
                    // mirror samples below/above the buffer's wrap-around point
                    size_t nFirstHalf = std::min(_size - index, n_slots_to_claim);
                    size_t nSecondHalf = n_slots_to_claim  - nFirstHalf;

                    std::memcpy(&_data[index + _size], &_data[index], nFirstHalf * sizeof(T));
                    if (nSecondHalf) {
                        std::memcpy(&_data[0], &_data[_size], nSecondHalf * sizeof(T));
                    }
                }
                _claimStrategy.publish(publishSequence); // points at first non-writable index
            } catch (...) {
                // blindly catch all exceptions from the user supplied translator function (i.e. unrelated to the buffer mechanics)
                // TODO: GR architects: is this an acceptable nominal behaviour, or
                // * should be thrown back to the user
                // * should be logged as a stack track
                // * ...
                // all the above have performance penalties (code-size, exception/log handling)
            }
        }
    };

    template<typename U>
    class buffer_host_reader
    {
        using BufferType = std::shared_ptr<buffer_impl>;

        alignas(kCacheLine) std::shared_ptr<Sequence>   _readIndex = std::make_shared<Sequence>();
        alignas(kCacheLine) Sequence&                   _readIndexRef;
        alignas(kCacheLine) std::int64_t                _readIndexCached;
        alignas(kCacheLine) BufferType                  _buffer; // controls buffer life-cycle, the rest are cache optimisations
        alignas(kCacheLine) const std::size_t           _size;
        alignas(kCacheLine) T*                          _data;
        alignas(kCacheLine) Sequence&                   _cursorRef;

    public:
        buffer_host_reader() = delete;
        buffer_host_reader(std::shared_ptr<buffer_impl> buffer) : _readIndexRef(*_readIndex),
            _buffer(buffer), _size(buffer->_size), _data(buffer->_data), _cursorRef(buffer->_cursor) {
            gr::detail::addSequences(_buffer->_readIndices, _buffer->_cursor, {_readIndex});
            _readIndexCached = _readIndex->value();
        }
        ~buffer_host_reader() { gr::detail::removeSequence( _buffer->_readIndices, _readIndex); }

        template <bool strict_check = true> //TODO: GR architectural default: safety vs. performance
        [[nodiscard]] constexpr std::span<const U> get(const std::size_t n_requested = 0) const noexcept {
            // TODO: adjust n_requested if U != T or via https://en.cppreference.com/w/cpp/container/span/as_bytes
            if constexpr (strict_check) {
                const std::size_t n = n_requested > 0 ? std::min(n_requested, available()) : available();
                return { &_data[_readIndexCached % _size], n };
            }
            const std::size_t n = n_requested > 0 ? n_requested : available();
            return { &_data[_readIndexCached % _size], n };
        }

        template <bool strict_check = true> //TODO: GR architectural default: safety vs. performance
        [[nodiscard]] constexpr bool consume(const std::size_t n_elements = 1) noexcept {
            // TODO: adjust n_requested if U != T or via https://en.cppreference.com/w/cpp/container/span/as_bytes
            if constexpr (strict_check) {
                if (n_elements <= 0) {
                    return true;
                }
                if (n_elements > available()) {
                    return false;
                }
            }
            _readIndexCached = _readIndexRef.addAndGet(n_elements);
            return true;
        }

        [[nodiscard]] constexpr std::size_t available() const noexcept { // TODO: adjust if U != T
            return _cursorRef.value() - _readIndexCached;
        }

        // alternate: sample-by-sample interface
        template <bool strict_check = false>
        [[nodiscard]] constexpr U& operator[](const std::int32_t index) const noexcept(strict_check) {
            if constexpr (strict_check) {
                if (index > available()) {
                    throw std::exception("...");
                }
                return _data[(_readIndexCached + _size + index) % _size];
            }
            return _data[(_readIndexCached + _size + index) % _size];
        };
    };

    std::shared_ptr<buffer_impl> _shared_buffer_ptr;

public:
    buffer_host() = delete;
    buffer_host(std::size_t min_size, Allocator allocator = Allocator(DoubleMappedAllocator::defaultAllocator()))
        : _shared_buffer_ptr(std::make_shared<buffer_impl>(min_size, allocator)) { }
    ~buffer_host() = default;

    [[nodiscard]] std::size_t       size() const noexcept { return _shared_buffer_ptr->_size; }

    template<typename WriteType = T>
    [[nodiscard]] BufferWriter auto newWriterInstance() { return buffer_host_writer<WriteType>(_shared_buffer_ptr); }

    template<typename ReadType = T>
    [[nodiscard]] BufferReader auto newReaderInstance() { return buffer_host_reader<ReadType>(_shared_buffer_ptr); }

    // implementation specific interface -- not part of public Buffer / production-code API
    [[nodiscard]] auto nReaders()       { return _shared_buffer_ptr->_readIndices->size(); }
    [[nodiscard]] auto claimStrategy()  { return _shared_buffer_ptr->_claimStrategy; }
    [[nodiscard]] auto waitStrategy()   { return _shared_buffer_ptr->_waitStrategy; }
    [[nodiscard]] auto cursorSequence() { return _shared_buffer_ptr->_cursor; }

};
static_assert(Buffer<buffer_host<int32_t>>);
// clang-format on

} // namespace gr

#endif // GNURADIO_BUFFER_HOST_HPP
