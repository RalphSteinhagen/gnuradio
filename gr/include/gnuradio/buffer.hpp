#ifndef GNURADIO_BUFFER2_H
#define GNURADIO_BUFFER2_H

#include <type_traits>
#include <concepts>
#include <span>
#include <tuple>

namespace gr {
namespace util {
template <typename T, typename...>
struct first_template_arg;

template <template <typename> class TemplateType, typename... ValueTypes>
    requires(sizeof...(ValueTypes) > 0)
struct first_template_arg<TemplateType<ValueTypes...>> {
    using value_type = std::tuple_element_t<0, std::tuple<ValueTypes...>>;
};

template <typename TemplateType>
using first_template_arg_v = typename first_template_arg<TemplateType>::value_type;

template <typename... A>
struct test {
};

static_assert(std::is_same_v<first_template_arg_v<test<int, float, double>>, int>);
static_assert(std::is_same_v<first_template_arg_v<std::tuple<int, double, float>>, int>);
} // namespace util

// clang-format off
// disable formatting until clang-format (v16) supporting concepts
template<class T>
concept BufferReader = requires(T /*const*/ t, const std::size_t n_items) {
    { t.get(n_items) }     -> std::same_as<std::span<const util::first_template_arg_v<T>>>;
    { t.consume(n_items) } -> std::same_as<bool>;
    { t.available() }      -> std::same_as<std::size_t>;
};

template<class T, typename ...Args>
concept BufferWriter = requires(T t, const std::size_t n_items, Args ...args) {
    { t.publish([](std::span<util::first_template_arg_v<T>> &writableData, Args ...args) {}, n_items, args...) } -> std::same_as<void>;
    { t.tryPublish([](std::span<util::first_template_arg_v<T>> &writableData, Args ...args){}, n_items, args...) } -> std::same_as<bool>;
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
