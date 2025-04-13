/*
 * Copyright 2020 Free Software Foundation, Inc.
 *
 * This file is part of GNU Radio
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 */

/* This file is automatically generated using bindtool */

#include <pybind11/complex.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

#include <gnuradio/vocoder/codec2.h>
// pydoc.h is automatically generated in the build directory
#include <codec2_pydoc.h>

void bind_codec2(py::module& m)
{

    using codec2 = ::gr::vocoder::codec2;

    py::class_<codec2, std::shared_ptr<codec2>> codec2_class(m, "codec2", D(codec2));

    py::enum_<gr::vocoder::codec2::bit_rate>(codec2_class, "bit_rate")
        .value("MODE_3200", gr::vocoder::codec2::MODE_3200)
        .value("MODE_2400", gr::vocoder::codec2::MODE_2400)
        .value("MODE_1600", gr::vocoder::codec2::MODE_1600)
        .value("MODE_1400", gr::vocoder::codec2::MODE_1400)
        .value("MODE_1300", gr::vocoder::codec2::MODE_1300)
        .value("MODE_1200", gr::vocoder::codec2::MODE_1200)
#ifdef CODEC2_MODE_700
        .value("MODE_700", gr::vocoder::codec2::MODE_700)
#endif
#ifdef CODEC2_MODE_700B
        .value("MODE_700B", gr::vocoder::codec2::MODE_700B)
#endif
#ifdef CODEC2_MODE_700C
        .value("MODE_700C", gr::vocoder::codec2::MODE_700C)
#endif
#ifdef CODEC2_MODE_WB
        .value("MODE_WB", gr::vocoder::codec2::MODE_WB)
#endif
#ifdef CODEC2_MODE_450
        .value("MODE_450", gr::vocoder::codec2::MODE_450)
#endif
#ifdef CODEC2_MODE_450PWB
        .value("MODE_450PWB", gr::vocoder::codec2::MODE_450PWB)
#endif
        .export_values();

    py::implicitly_convertible<int, gr::vocoder::codec2::bit_rate>();
}
