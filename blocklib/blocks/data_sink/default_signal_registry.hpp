/*
 * Copyright 2022 FAIR - Facility for Anti-Proton and Ion Research, Germany
 *
 * This file is part of GNU Radio
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
#ifndef GNURADIO_DEFAULT_SIGNAL_REGISTRY_HPP
#define GNURADIO_DEFAULT_SIGNAL_REGISTRY_HPP
#include "signal_registry.hpp"

#include <mutex>

#include <gnuradio/sequence.hpp>

namespace gr::blocks {


class default_signal_registry : public signal_registry
{
    std::mutex _lock;
    std::vector<std::reference_wrapper<data_sink_t>> _registry;
    const std::string _registry_name;
    Sequence _changed_config;

public:
    default_signal_registry() = delete;
    default_signal_registry(const std::string_view registry_name)
        : signal_registry(), _registry_name(registry_name){};
    [[nodiscard]] static std::shared_ptr<signal_registry> get_shared_instance()
    {
        static std::shared_ptr<signal_registry> self =
            std::make_shared<default_signal_registry>("default_signal_registry");
        return self;
    }

    [[nodiscard]] std::string_view name() { return _registry_name; }
    [[nodiscard]] std::size_t size() const noexcept { return _registry.size(); }

    data_sink_t& find(const std::string_view& sink_name)
    {
        std::lock_guard guard(_lock);
        for (auto& sink : _registry) {
            //TODO: iterate over multiple available signals
            if (sink.get().signal_infos()[0].signal_name == sink_name) {
                fmt::print("found sink {}\n", sink_name);
            }
            return sink;
        }
        fmt::print("did not find sink {}\n", sink_name);
        throw fmt::format("did not find sink {}", sink_name);
    }

    [[nodiscard]] bool has_changed(Sequence& last_update) const noexcept
    {
        if (_changed_config.value() == last_update.value()) {
            return false;
        }
        else {
            last_update.setValue(_changed_config.value());
            return true;
        }
    };

    //    buffer_reader_t get_reader(const std::string_view& sink_name)
    //    { // implements poll-API
    //        std::lock_guard guard(_lock);
    //        for (auto& sink : _registry) {
    //            if (sink.get().name() == sink_name) {
    //                fmt::print("found sink {} -> returning reader of type_index {}\n",
    //                           sink_name,
    //                           sink.get().type().index());
    //                std::cout << "sink type " << typeid(decltype(sink.get())).name() <<
    //                '\n'; return std::visit(
    //                    [&sink, &sink_name](auto&& arg) {
    //                        using T = std::decay_t<decltype(arg)>;
    //                        std::cout << "type " << typeid(T).name() << '\n';
    //                        if constexpr (std::is_same_v<T, std::monostate>) {
    //                            return buffer_reader_t(std::monostate());
    //                        }
    //                        else {
    //                            fmt::print("cast to specific sink -> return reader {} -
    //                            "
    //                                       "data_sink<{}>\n",
    //                                       sink_name,
    //                                       typeid(T).name());
    //                            // data_sink_cpu<T>& typed_sink =
    //                            // static_cast<data_sink_cpu<T>&>(sink.get()); return
    //                            // typed_sink.get_read_buffer();
    //                            return buffer_reader_t(sink.get().get_read_buffer());
    //                            // return
    //                            //
    //                            buffer_reader_t(static_cast<data_sink_cpu<T>&>(sink.get()).get_read_buffer());
    //                            // auto reader = buffer<T>().new_reader();
    //                            // return buffer_reader_t(reader);
    //                        }
    //                    },
    //                    sink.get().type());
    //            }
    //        }
    //        fmt::print("did not find sink/no reader {}\n", sink_name);
    //        return buffer_reader_t(std::monostate());
    //    }


    [[maybe_unused]] std::int64_t add(data_sink_t& data_sink) noexcept override
    {
        std::lock_guard guard(_lock);
        _registry.push_back(std::ref(data_sink));

        fmt::print("after add size = {}\n", _registry.size());
        return _changed_config.incrementAndGet();
    }
    [[maybe_unused]] std::int64_t remove(data_sink_t& data_sink) noexcept override
    {
        std::lock_guard guard(_lock);
        auto cond = [&data_sink](const std::reference_wrapper<data_sink_t>& e) {
            return &e.get() == &data_sink;
        };
        _registry.erase(std::remove_if(_registry.begin(), _registry.end(), cond),
                        _registry.end());
        ;

        fmt::print("after remove size = {}\n", _registry.size());
        return _changed_config.incrementAndGet();
    }
};
static_assert(SignalRegistry<default_signal_registry>);

} // namespace gr::blocks

#endif // GNURADIO_DEFAULT_SIGNAL_REGISTRY_HPP
