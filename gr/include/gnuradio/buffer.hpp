#ifndef GNURADIO_BUFFER2_H
#define GNURADIO_BUFFER2_H

#include <type_traits>
#include <concepts>
#include <span>

namespace gr {
namespace util {
template <typename T, typename...>
struct first_template_arg_helper;

template <template <typename...> class TemplateType,
          typename ValueType,
          typename... OtherTypes>
struct first_template_arg_helper<TemplateType<ValueType, OtherTypes...>> {
    using value_type = ValueType;
};

template <typename T>
constexpr auto* value_type_helper()
{
    if constexpr (requires { typename T::value_type; }) {
        return static_cast<typename T::value_type*>(nullptr);
    }
    else {
        return static_cast<typename first_template_arg_helper<T>::value_type*>(nullptr);
    }
}

template <typename T>
using value_type_t = std::remove_pointer_t<decltype(value_type_helper<T>())>;

template <typename... A>
struct test_fallback {
};

template <typename, typename ValueType>
struct test_value_type {
    using value_type = ValueType;
};

static_assert(std::is_same_v<int, value_type_t<test_fallback<int, float, double>>>);
static_assert(std::is_same_v<float, value_type_t<test_value_type<int, float>>>);
static_assert(std::is_same_v<int, value_type_t<std::span<int>>>);
static_assert(std::is_same_v<double, value_type_t<std::array<double, 42>>>);
} // namespace util

// clang-format off
// disable formatting until clang-format (v16) supporting concepts
template<class T>
concept BufferReader = requires(T /*const*/ t, const std::size_t n_items) {
    { t.get(n_items) }     -> std::same_as<std::span<const util::value_type_t<T>>>;
    { t.consume(n_items) } -> std::same_as<bool>;
    { t.available() }      -> std::same_as<std::size_t>;
};

template<class T, typename ...Args>
concept BufferWriter = requires(T t, const std::size_t n_items, Args ...args) {
    { t.publish([](std::span<util::value_type_t<T>> &writableData, Args ...args) {}, n_items, args...) } -> std::same_as<void>;
    { t.tryPublish([](std::span<util::value_type_t<T>> &writableData, Args ...args){}, n_items, args...) } -> std::same_as<bool>;
    { t.available() }         -> std::same_as<std::size_t>;
};

template<class T, typename U1 = T, typename U2 = T>
concept Buffer = requires(T t, const std::size_t min_size) {
    { T(min_size) };
    { t.size() }              -> std::same_as<std::size_t>;
    { t.newReaderInstance() } -> BufferReader;
    { t.newWriterInstance() } -> BufferWriter;
};

// clang-format on

namespace test {
template <typename T>
struct non_compliant_class {
};
} // namespace test
static_assert(!Buffer<test::non_compliant_class<int>>);
static_assert(!BufferReader<test::non_compliant_class<int>>);
static_assert(!BufferWriter<test::non_compliant_class<int>>);

} // namespace gr

#endif // GNURADIO_BUFFER2_H
