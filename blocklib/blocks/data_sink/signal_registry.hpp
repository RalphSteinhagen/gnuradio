/*
 * Copyright 2022 FAIR - Facility for Anti-Proton and Ion Research, Germany
 *
 * This file is part of GNU Radio
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef GNURADIO_SIGNAL_REGISTRY_HPP
#define GNURADIO_SIGNAL_REGISTRY_HPP

#include <string_view>
#include <type_traits>
#include <complex>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <gnuradio/buffer.hpp>
#include <gnuradio/circular_buffer.hpp>
#include <gnuradio/sequence.hpp>
#include <gnuradio/tag.h>
#include <pmtv/pmt.hpp>

// clang-format off
namespace gr::blocks {

namespace detail {
template<template<typename... > class VariantType, template<typename... > class SinkType, typename... Args>
struct as_data_sink_ref_variant { using type = VariantType<std::monostate, std::reference_wrapper<SinkType<Args>>...>; };
template<template<typename... > class VariantType, template<typename... > class SinkType, typename... T>
struct as_data_sink_ref_variant<VariantType, SinkType, std::tuple<T...>> { using type = typename as_data_sink_ref_variant<VariantType, SinkType, T...>::type; };

template<template<typename... > class VariantType, typename... Args>
struct as_buffer_variant { using type = VariantType<std::monostate, circular_buffer<Args>...>; };
template<template<typename... > class VariantType, typename... T>
struct as_buffer_variant<VariantType, std::tuple<T...>> { using type = typename as_buffer_variant<VariantType, T...>::type; };

template<typename T> using buffer_reader_t = decltype(circular_buffer<T>(0).new_reader());
template<typename T> using buffer_writer_t = decltype(circular_buffer<T>(0).new_writer());
template<template<typename... > class VariantType, typename... Args>
struct as_read_buffer_variant { using type = VariantType<std::monostate, buffer_reader_t<Args>...>; };
template<template<typename... > class VariantType, typename... T>
struct as_read_buffer_variant<VariantType, std::tuple<T...>> { using type = typename as_read_buffer_variant<VariantType, T...>::type; };

template<template<typename... > class VariantType, template<typename... > class CallbackType, typename... Args>
struct as_typed_variant { using type = VariantType<std::monostate, CallbackType<Args>...>; };
template<template<typename... > class VariantType, template<typename... > class CallbackType, typename... T>
struct as_typed_variant<VariantType, CallbackType, std::tuple<T...>> { using type = typename as_typed_variant<VariantType, CallbackType, T...>::type; };

template<template<typename... > class VariantType, template<typename... > class CallbackType, typename... Args>
struct as_typed_shared_variant { using type = VariantType<std::monostate, std::shared_ptr<CallbackType<Args>>...>; };
template<template<typename... > class VariantType, template<typename... > class CallbackType, typename... T>
struct as_typed_shared_variant<VariantType, CallbackType, std::tuple<T...>> { using type = typename as_typed_shared_variant<VariantType, CallbackType, T...>::type; };
} // namespace detail

template<typename T> class data_sink_cpu;
template<typename T> class data_reader;

using data_sink_ref_t = detail::as_data_sink_ref_variant<std::variant, data_sink_cpu, pmtv::default_supported_types>::type;
using data_reader_t = detail::as_typed_shared_variant<std::variant, data_reader, pmtv::default_supported_types>::type;
using buffer_reader_t = detail::as_read_buffer_variant<std::variant, pmtv::default_supported_types>::type;
using buffer_t = detail::as_buffer_variant<std::variant, pmtv::default_supported_types>::type;

template<typename T>
[[nodiscard]] int data_sink_call_back(const std::span<std::span<const T>>& data, const std::span<std::span<const T>>& errors, const std::size_t buffer_size,
                                const std::span<tag_t>& tags,
                                bool config_changed, const data_reader<T>& config) {

    return 0; // number of to be consumed elements, e.g. either all, in relation to buffer_size and data size, ...
}
// Q: how to pass this to a virtual 'data_sink' interface efficiently, see example below.
template<typename T>
using data_sink_callback_t = std::function<int(const std::span<std::span<const T>>&, const std::span<std::span<const T>>&, std::size_t, const std::span<tag_t>&, bool, const data_reader<T>&)>;
using data_sink_var_callback_t = detail::as_typed_variant<std::variant, data_sink_callback_t, pmtv::default_supported_types>::type;

using signal_info_t = struct signal_info {
    const std::string signal_name;
    const std::string signal_unit;
    const float signal_rate;
    const pmtv::pmt_var_t signal_type;
};

using data_sink_t = struct data_sink_base {
    virtual ~data_sink_base() = default;
    [[nodiscard]] virtual bool        has_config_changed(Sequence& last_update) const noexcept = 0; // true: -> query RT-info: name, unit, ... again
    [[nodiscard]] virtual std::vector<signal_info_t> signal_infos() const noexcept = 0;
    [[nodiscard]] virtual pmtv::pmt_var_t   data_type() const noexcept = 0; // static constexpr information, cannot change sink's value_type during runtime
    // register call-back function -- Q: howto make this typed
    [[nodiscard]] virtual data_reader_t register_reader(std::span<std::string_view> /* signal_names */,
                                                          std::size_t /* min_buffer_size */, bool /* blocking */, data_sink_var_callback_t&,
                                                          std::size_t /* min_notification */, std::size_t /* max_notification */) noexcept = 0;
};

template<typename T>
concept SignalRegistry = requires (T t, data_sink_t& sink) {
    { t.add(sink) }    -> std::same_as<std::int64_t>;
    { t.remove(sink) } -> std::same_as<std::int64_t>;
    { t.size() }       -> std::same_as<std::size_t>;
};

class signal_registry { // this virtual abstraction is primarily needed to allow for implementations other than 'default_signal_registry'
   public:
    signal_registry() {}
    virtual ~signal_registry() {}
    virtual std::int64_t add(data_sink_t&) noexcept = 0;
    virtual std::int64_t remove(data_sink_t&) noexcept = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
};

} // namespace gr::blocks
// clang-format on

#endif // GNURADIO_SIGNAL_REGISTRY_HPP
