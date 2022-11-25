/*
 * Copyright 2022 FAIR - Facility for Anti-Proton and Ion Research, Germany
 *
 * This file is part of GNU Radio
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef GNURADIO_DATA_SINK_HPP
#define GNURADIO_DATA_SINK_HPP

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <fmt/core.h>
#include <fmt/format.h>
#include <fmt/ostream.h>

#include <gnuradio/blocks/data_sink.h>
#include <gnuradio/circular_buffer.hpp>
#include <gnuradio/tag.h>
#include <pmtv/pmt.hpp>
#include <volk/volk.h>

#include "default_signal_registry.hpp"

namespace gr {
namespace blocks {

/**
 * @brief generic data sink for exporting arbitrary-typed streams to non-GR C++ APIs.
 *
 * Each sink registers with a (user-defined/exchangeable) global registry that can be
 * queried by the non-GR caller to find the sink responsible for a given signal name, etc.
 * and either retrieve a
 * * poller handler that allows asynchronous data from a different thread, or
 * * register a callback that is invoked by the sink if the user-conditions are met.
 *
 * <pre>
 * @code
 *         ╔═══════════════╗
 *    in0 ━╢   data sink   ║                      ┌──── caller ────┐
 * (err0) ━╢ (opt. error)  ║                      │                │
 *    ┄    ║               ║  retrieve poller or  │ (custom non-GR │
 *    inN ━╢ :signal_names ║←--------------------→│  user code...) │
 * (errN) ━╢ :signal_units ║  register            │                │
 *         ║ :...          ║  callback function   └───┬────────────┘
 *         ╚═ GR block ═╤══╝                          │
 *                      │                             │
 *                      │                             │
 *                      │      ╭─registry─╮           │
 *            register/ │      ╞══════════╡           │ queries for specific
 *          deregister  ╰─────→│ [sinks]  │←──────────╯ signal_info_t list/criteria
 *                             ╞══════════╡
 *                             ╰──────────╯
 *
 * </pre>
 * Both poller and callback can be configured to be blocking, i.e. blocks the flow-graph
 * if data is not being retrieved in time, or non-blocking, i.e. data being dropped when
 * the user-defined buffer size is full.
 * N.B. due to the nature of the GR scheduler, signals from the same sink are notified
 * synchronuously (/asynchronuously) if handled by the same (/different) sink block.
 *
 * @tparam T input port parameter (N.B. need to be the same
 */
template <class T>
class data_sink_cpu : public data_sink<T>, public data_sink_t
{
    std::mutex _lock;
    std::vector<T> _data;
    std::vector<tag_t> _tags;
    size_t _vlen;
    std::shared_ptr<signal_registry> _registry;
    Sequence _changed_config;
    std::vector<std::shared_ptr<data_reader<T>>> _reader;
    bool _with_errors = true;

public:
    using value_type = T;
    // data_sink_cpu() = delete;
    // data_sink_cpu(data_sink_cpu&) = delete;
    // data_sink_cpu& operator=(data_sink_cpu&) = delete:
    data_sink_cpu(const typename data_sink<T>::block_args& args,
                  std::shared_ptr<signal_registry> registry =
                      default_signal_registry::get_shared_instance())
        : sync_block("data_sink", "blocks"),
          data_sink<T>(args),
          data_sink_t(),
          _vlen(args.vlen),
          _registry(registry)
    {
        _data.reserve(_vlen * args.reserve_items);
        _registry->add(*this);
    }

    ~data_sink_cpu() { _registry->remove(*this); }

    [[nodiscard]] bool has_changed(Sequence& last_update) const noexcept
    {
        if (_changed_config.value() == last_update.value()) {
            return false;
        }
        last_update.setValue(_changed_config.value());
        return true;
    };

    // TODO: add hooks to actual block parameter
    [[nodiscard]] std::vector<signal_info_t> signal_infos() const noexcept override
    {
        return { { "my name", "", -1.0f, T() } };
    }
    [[nodiscard]] constexpr pmtv::pmt_var_t data_type() const noexcept override
    {
        return value_type();
    }
    [[nodiscard]] bool has_config_changed(Sequence& last_update) const noexcept override
    {
        auto actual_value = _changed_config.value();
        if (actual_value == last_update.value()) {
            return false;
        }
        last_update.setValue(actual_value);
        return true;
    };

    [[nodiscard]] data_reader_t
    register_reader(std::span<std::string_view> signals,
                    std::size_t min_buffer_size,
                    bool blocking,
                    data_sink_var_callback_t& callback_function,
                    std::size_t /* min_notification */,
                    std::size_t /* max_notification */) noexcept override
    {
        using reader_t = data_reader<T>;
        std::scoped_lock lock(_lock);
        std::vector<std::size_t> port_indices;
        auto new_reader = std::make_shared<reader_t>(min_buffer_size, blocking, _with_errors, port_indices, nullptr);
        _reader.push_back(new_reader);
        return new_reader;
    }

    void specific_method() { fmt::print("access specific_method({})\n", fmt::ptr(this)); }

    // on_parameter_query is overridden here because PMT currently does not
    // support element push_back of pmtv::vector.  So it is very inefficient
    // to deal with the pmt directly in the work function.  Just work with the
    // private member variable, and pass it out as pmt when queried
    void on_parameter_query(param_action_sptr action) override
    {
        this->d_debug_logger->trace(
            "block {}: on_parameter_query param_id: {}", this->id(), action->id());
        pmtv::pmt_var_t param = _data;
        // auto data = pmtv::get_as<std::vector<float>>(*param);
        action->set_pmt_value(param);
    }

    work_return_t work(work_io& wio) override
    {
        int port = 0;
        for (auto& input : wio.inputs()) {
            // auto iptr = input.items<T>();
            int n_items = input.n_items;

            fmt::print("port{}: consume {} samples - value[0] = \n", port, n_items);
            input.n_consumed = n_items;
            port++;
        }
        auto iptr = wio.inputs()[0].items<T>();
        int noutput_items = wio.inputs()[0].n_items;

        for (unsigned int i = 0; i < noutput_items * _vlen; i++)
            _data.push_back(iptr[i]);

        auto tags = wio.inputs()[0].tags_in_window(0, noutput_items);
        _tags.insert(_tags.end(), tags.begin(), tags.end());

        wio.consume_each(noutput_items);
        return work_return_t::OK;
    }
};

template <typename T>
class data_reader
{
    const std::vector<std::size_t> _port_index;
    const bool _is_blocking = true;
    bool _with_errors = true;
    const std::vector<std::size_t> _port_indices;
    std::atomic<bool> _disconnect = false;
    std::vector<circular_buffer<T>> _buffer;
    std::vector<detail::buffer_reader_t<T>> _buffer_readers;
    std::vector<detail::buffer_writer_t<T>> _buffer_writers;
    std::map<std::string, pmtv::pmt_var_t> _config;
    Sequence _changed_config;
    Sequence _last_update;
    std::vector<std::span<T>> _data;
    std::vector<std::span<T>> _errors;
    const data_sink_callback_t<T> _callback;

public: // TODO: remove
        //    data_reader() = delete;
    data_reader() = default;
    //    data_reader(data_reader<T>&& other) { *this = std::move(other); };
    //    auto& operator=(data_reader<T>&&) {  };
    data_reader(const std::size_t min_buffer,
                const bool is_blocking,
                bool with_errors,
                std::vector<std::size_t>& port_indices,
                const data_sink_callback_t<T>& callback = nullptr)
        : _is_blocking(is_blocking),
          _with_errors(with_errors),
          _port_indices(port_indices),
          _data(_port_indices.size()),
          _errors(_port_indices.size()),
          _callback(callback == nullptr ? nullptr : callback)
    {
        for (auto port_id = 0U; port_id < _port_indices.size(); port_id++) {
            _buffer.push_back(circular_buffer<T>(min_buffer));
            _buffer_readers.push_back(_buffer.at(port_id).new_reader());
            _buffer_writers.push_back(_buffer.at(port_id).new_writer());
        }
    }

    void notify() const noexcept
    {
        if (!_callback) {
            return;
        }
    }

    friend data_sink_cpu<T>;

public:
    [[nodiscard]] std::vector<signal_info_t> signal_infos() const noexcept
    {
        return { { "my name", "", -1.0f, T() } };
    }

    [[nodiscard]] const std::vector<detail::buffer_reader_t<T>>&
    buffer_reader() const noexcept
    {
        return _buffer_readers;
    }
    [[nodiscard]] const std::map<std::string, pmtv::pmt_var_t> config() const noexcept
    {
        return _config;
    };
    void disconnect() const noexcept { _disconnect = false; }
    [[nodiscard]] bool has_changed() const noexcept { return has_changed(_last_update); }
    [[nodiscard]] bool has_changed(Sequence& last_update) const noexcept
    {
        if (_changed_config.value() == last_update.value()) {
            return false;
        }
        last_update.setValue(_changed_config.value());
        return true;
    };


    friend class data_sink_cpu<T>;
};

} // namespace blocks
} // namespace gr

#endif // GNURADIO_DATA_SINK_HPP