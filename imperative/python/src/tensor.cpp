/**
 * \file imperative/python/src/tensor.cpp
 * MegEngine is Licensed under the Apache License, Version 2.0 (the "License")
 *
 * Copyright (c) 2014-2020 Megvii Inc. All rights reserved.
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT ARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#include "megbrain/dtype.h"
#include "megbrain/common.h"
#include "megbrain/imperative/ops/utility.h"

#include "./tensor.h"
#include "./grad.h"
#include "./trace.h"
#include "./common.h"
#include "./numpy_dtypes.h"
#include "./graph_rt.h"
#include "./helper.h"

#include <pybind11/numpy.h>
#include <pybind11/operators.h>
#include <range/v3/all.hpp>

#include <unordered_map>

namespace py = pybind11;
namespace views = ranges::views;

namespace mgb::imperative::python {

std::unique_ptr<interpreter::Interpreter::Channel> interpreter_for_py;

PyObject *cpp_apply_with_tracing, *cpp_apply_const_with_tracing,
           *cpp_apply_compiled_mode, *cpp_apply_const_compiled_mode;

PyObject *cpp_apply_backward_varnode;


#define REGISTE_APPLY_FUNC(mode)                                    \
        void set_##mode(py::object pyf) {                           \
            mode = pyf.ptr();                                       \
        }

REGISTE_APPLY_FUNC(cpp_apply_with_tracing)
REGISTE_APPLY_FUNC(cpp_apply_const_with_tracing)
REGISTE_APPLY_FUNC(cpp_apply_compiled_mode)
REGISTE_APPLY_FUNC(cpp_apply_const_compiled_mode)
REGISTE_APPLY_FUNC(cpp_apply_backward_varnode)

#undef REGISTE_APPLY_FUNC

bool is_tracing = false;
bool is_symbolic = false;
bool is_compiled = false;

#define SET_UNSET_PROP(mode)    \
    void set_##mode() {         \
        is_##mode = true;       \
    }                           \
    void unset_##mode() {       \
        is_##mode = false;      \
    }                           \

SET_UNSET_PROP(tracing)
SET_UNSET_PROP(symbolic)
SET_UNSET_PROP(compiled)

#undef SET_UNSET_PROP

bool skip_tracing = false;

Tensor::flags_t ApplyContext::global_disable = 0;

apply_result_t apply(ApplyContext& ctx) {
    // emulating scalar should be put to specific op's apply, e.g.,
    // elementwise, reduce, typecvt. Currently it's still handled at python
    // side. It could be move to C++ side if it has an impact on performance
    auto flags = ctx.flags & ~ApplyContext::global_disable;

    if (flags & Tensor::Flags::SCALAR) {
        // TODO: emulate scalar
    }

    if (flags & Tensor::Flags::GRAD) {
        return apply_grad(ctx);
    }

    if (auto* op = ctx.op->try_cast_final<GenericPyOp>()) {
        py::tuple pyin(ctx.nargs);
        for (size_t i = 0; i < ctx.nargs; ++i) {
            pyin[i] = TensorWrapper::make(ctx.pytype, ctx.args[i]->shared_from_this());
        }
        auto f = py::getattr(op->obj, "_default_rule");
        auto pyout = py::reinterpret_steal<py::object>(PyObject_Call(f.ptr(), pyin.ptr(), nullptr));
        if (!pyout) throw py::error_already_set();
        if (auto* tw = TensorWrapper::try_cast(pyout.ptr())) {
            return {tw->m_tensor};
        }
        apply_result_t ret;
        ret.reserve(py::len(pyout));
        for (auto&& i : pyout) {
            auto* tw = TensorWrapper::try_cast(i.ptr());
            mgb_assert(tw);
            ret.push_back(tw->m_tensor);
        }
        return ret;
    }

    if (flags & Tensor::Flags::TRACE) {
        return apply_trace(ctx);
    } else {
        SmallVector<interpreter::Interpreter::Handle> handles(ctx.nargs);
        for (size_t i = 0; i < ctx.nargs; ++i) {
            handles[i] = ctx.args[i]->m_handle.get();
        }

        auto output_handles = interpreter_for_py->apply_op(ctx.op, handles);

        apply_result_t outputs;
        outputs.reserve(output_handles.size());
        for (auto h : output_handles) {
            outputs.emplace_back(std::make_shared<Tensor>(h));
        }
        return outputs;
    }

    mgb_assert(0);
}

PyObject* py_apply(PyObject* self, PyObject*const* args, size_t nargs/* , PyObject* kwnames */) {
    try {
        // if (kwnames && PyTuple_GET_SIZE(kwnames)) {
        //     PyErr_SetString(PyExc_TypeError, "keyword argument not allowed");
        //     return nullptr;
        // }
        if (nargs < 2) {
            PyErr_SetString(PyExc_TypeError,
                            "py_apply expects one Op and at least one tensor "
                            "as argument");
            return nullptr;
        }

        auto* op = args[0];

        PyTypeObject* pytype = args[1]->ob_type;
        ++args;
        --nargs;

        ApplyContext ctx;
        ctx.flags = 0;
        ctx.op = py::handle(op).cast<std::shared_ptr<OpDef>>();
        SmallVector<Tensor*, 64> tensors(nargs);
        ctx.args = &tensors[0];
        ctx.nargs = nargs;
        ctx.pytype = pytype;
        if (strstr(op->ob_type->tp_name, "BackwardGraph")) {
            ctx.backward = true;
        }

        for (size_t i = 0; i < nargs; ++i) {
            if (TensorWrapper* tw = TensorWrapper::try_cast(args[i])) {
                auto* t = tensors[i] = tw->m_tensor.get();
                ctx.flags |= t->m_flags;
            } else {
                PyErr_SetString(PyExc_TypeError, "expect Tensor");
                return nullptr;
            }
        }

        if (is_tracing) {
            ctx.flags |= Tensor::Flags::TRACE;
        }

        auto outputs = apply(ctx);
        size_t nout = outputs.size();
        auto ret = py::tuple(nout);
        for (size_t i = 0; i < nout; ++i) {
            ret[i] = TensorWrapper::make(pytype, std::move(outputs[i]));
        }
        return ret.release().ptr();
    } catch (std::exception& e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }
}


TensorWrapper::TensorWrapper(PyObject* args, PyObject* kwargs) {
    if (kwargs && PyDict_Size(kwargs)) {
        throw py::type_error("keyword argument not allowed");
    }
    auto nargs = PyTuple_Size(args);
    auto tup = py::reinterpret_borrow<py::tuple>(args);
    if (nargs == 0) {
        throw py::type_error("too few arguments");
    }
    if (auto* t = try_cast(tup[0].ptr())) {
        if (nargs > 1) {
            throw py::type_error("expect 1 argument");
        }
        m_tensor = t->m_tensor;
    } else {
        if (nargs == 1) {
            auto arg0 = PyTuple_GetItem(args, 0);
            // for lazy_eval_tensor
            if (strstr(arg0->ob_type->tp_name, "VarNode")) {
                if (PyObject_HasAttrString(arg0, "_node")) {
                    arg0 = PyObject_GetAttrString(arg0, "_node");
                }
                m_tensor = std::make_shared<Tensor>(py::handle(arg0).cast<cg::VarNode *>());
            } else {
                // for DeviceTensorND
                if (strstr(arg0->ob_type->tp_name, "DeviceTensorND")) {
                    auto dv = py::handle(arg0).cast<DeviceTensorND>();
                    interpreter::Interpreter::Handle handle = interpreter_for_py->put(dv);
                    m_tensor = std::make_shared<Tensor>(handle);
                } else {
                    throw py::type_error("single argument is not tensor, varnode or devicetensor");
                }
            }
        } else {
            py::detail::loader_life_support life_sup; // FIXME!!!required to cast DType
            if (nargs != 4 && nargs != 5) {
                throw py::type_error("expect 4 or 5 arguments");
            }
            auto data = tup[0].cast<py::array>();
            DType dtype = tup[1].cast<DType>();
            CompNode cn = tup[2].cast<CompNode>();
            bool is_const = tup[3].cast<bool>();
            bool no_cache = nargs == 5 ? tup[4].cast<bool>() : false;

            // const op
            if (is_const && is_tracing) {
                PyObject *pyf;
                if (is_compiled) {
                    pyf = cpp_apply_const_compiled_mode;
                } else {
                    pyf = cpp_apply_const_with_tracing;
                }

                auto ret = py::reinterpret_steal<py::object>(
                        PyObject_Call(pyf, tup.ptr(), nullptr));
                auto py_ret = py::reinterpret_borrow<py::list>(ret);
                if (auto* t = try_cast(py_ret[0].ptr())) {
                    m_tensor = t->m_tensor;
                }
                return;
            }

            interpreter::Interpreter::Handle handle;
            constexpr auto size_threshhold = TensorShape::MAX_NDIM;
            if (data.size() > size_threshhold) {
                handle = interpreter_for_py->put(npy::np2tensor(data.ptr(), npy::Meth::borrow(cn), dtype), no_cache);
            } else {
                HostTensorND ret(cn);
                handle = interpreter_for_py->put(npy::np2tensor(data.ptr(), npy::Meth::copy_into(&ret), dtype), no_cache);
            }

            m_tensor = std::make_shared<Tensor>(handle);

            if (data.ndim() == 0) {
                m_tensor->m_flags |= Tensor::Flags::SCALAR;
            }
        }
    }
}


#define REGISTE_TENSORWRAPPER_FUNC(type, member)                                    \
        PyObject* TensorWrapper::member() {                                         \
            return py::cast(m_tensor->m_trace_info.member).release().ptr();         \
        }                                                                           \
        void TensorWrapper::set_##member(PyObject* dest) {                          \
            auto py_dest = py::reinterpret_borrow<py::object>(dest);                \
            type real_dest = py_dest.cast<type>();                                  \
            m_tensor->m_trace_info.member = real_dest;                              \
        }

REGISTE_TENSORWRAPPER_FUNC(bool, data_read)
REGISTE_TENSORWRAPPER_FUNC(bool, value_read)
REGISTE_TENSORWRAPPER_FUNC(bool, shape_read)
REGISTE_TENSORWRAPPER_FUNC(int64_t, mixin_handle)

#undef REGISTE_TENSORWRAPPER_FUNC


PyObject* TensorWrapper::handle() {
    return py::cast(m_tensor->m_handle).release().ptr();
}


void TensorWrapper::set_handle(PyObject* dest) {
    auto py_dest = py::reinterpret_borrow<py::object>(dest);
    SharedHandle real_dest = py_dest.cast<SharedHandle>();
    m_tensor->m_handle = std::move(real_dest);
}


PyObject* TensorWrapper::shape() {
    if (!skip_tracing) {
        set_shape_read(py::cast(true).  release().ptr());
    }
    if (m_tensor->m_flags & Tensor::Flags::SCALAR) {
        return PyTuple_New(0);
    }

    TensorShape shape;
    if (m_tensor->m_var) {
        shape = m_tensor->m_var->shape();
    } else {
        shape = m_tensor->shape();
    }

    if (!shape.ndim) {
        Py_RETURN_NONE;
    }
    py::tuple ret(shape.ndim);
    for (size_t i = 0; i < shape.ndim; ++i) {
        ret[i] = shape[i];
    }
    return ret.release().ptr();
}


PyObject* TensorWrapper::dtype() {
    if (m_tensor->m_var) {
        return py::cast(m_tensor->m_var->dtype()).release().ptr();
    }
    return py::cast(m_tensor->dtype()).release().ptr();
}


PyObject* TensorWrapper::device() {
    if (m_tensor->m_var) {
        return py::cast(m_tensor->m_var->comp_node()).release().ptr();
    }
    return py::cast(m_tensor->comp_node()).release().ptr();
}


PyObject* TensorWrapper::numpy() {
    if (!skip_tracing) {
        set_value_read(py::cast(true).release().ptr());
    }
    if (m_tensor->m_handle.get() == nullptr && m_tensor->m_var != nullptr) {
        auto&& mgr = m_tensor->m_var->owner_graph()->static_infer_manager();
        auto&& type = mgr.get_infer_type(m_tensor->m_var);
        using InferType = cg::static_infer::InferType;
        if (!(type.value & (InferType::CONST | InferType::RT_STATIC))) {
            PyErr_SetString(PyExc_ValueError, "tensor invalid");
            return nullptr;
        }
        auto* val = mgr.infer_value_fallible(m_tensor->m_var);
        if (!val) {
            PyErr_SetString(PyExc_ValueError, "tensor invalid");
            return nullptr;
        }
        return py::cast(*val).attr("numpy")().release().ptr();
    }
    auto&& hv = interpreter_for_py->get_value(m_tensor->m_handle.get());
    auto arr = py::reinterpret_steal<py::array>(npy::ndarray_from_tensor(hv, npy::ShareType::TRY_SHARE));
    if (!arr) {
        PyErr_SetString(PyExc_ValueError, "tensor invalid");
        return nullptr;
    }
    if (m_tensor->m_flags & Tensor::Flags::SCALAR) {
        mgb_assert(PyArray_Check(arr.ptr()));
        return PyArray_Squeeze(reinterpret_cast<PyArrayObject*>(arr.ptr()));
    }
    return arr.release().ptr();
}

PyObject* TensorWrapper::varnode() {
    if (m_tensor->m_var) {
        return py::cast(m_tensor->m_var).release().ptr();
    }
    return py::none().release().ptr();
}

void TensorWrapper::reset(PyObject* tensor) {
    TensorWrapper* t = TensorWrapper::try_cast(tensor);
    if (!t) {
        throw py::type_error("expect Tensor");
    }
    m_tensor = t->m_tensor;
}

void TensorWrapper::reset_varnode() {
    m_tensor->m_var = nullptr;
}

PyObject* TensorWrapper::detach() {
    PyObject* self = wrap_t::pycast(this);
    PyTypeObject* pytype = self->ob_type;

    std::shared_ptr<Tensor> new_tensor;
    if (m_tensor->m_handle.get()) {
        new_tensor = std::make_shared<Tensor>(m_tensor->m_handle);
    } else {
        new_tensor = std::make_shared<Tensor>(m_tensor->m_var);
    }
    new_tensor->m_trace_info = m_tensor->m_trace_info;
    auto ret = TensorWrapper::make(pytype, std::move(new_tensor));
    return ret.release().ptr();

}

PyObject* TensorWrapper::_dev_tensor(){
    if (!skip_tracing) {
        set_data_read(py::cast(true).release().ptr());
    }
    auto dev_tensor = interpreter_for_py->get_dev_tensor(m_tensor->m_handle.get());
    return py::cast(dev_tensor).release().ptr();
}

void TensorWrapper::_swap_out() {
    interpreter_for_py->swap_out(m_tensor->m_handle.get());
}

void TensorWrapper::_swap_in() {
    interpreter_for_py->swap_in(m_tensor->m_handle.get());
}

void TensorWrapper::_drop() {
    interpreter_for_py->drop(m_tensor->m_handle.get());
}


PyObject* TensorWrapper::isscalar() {
    if(m_tensor->m_flags & Tensor::Flags::SCALAR) {
        Py_RETURN_TRUE;
    } else {
        Py_RETURN_FALSE;
    }
}


void TensorWrapper::setscalar() {
    m_tensor->m_flags |= Tensor::Flags::SCALAR;
}


struct TensorWeakRef {
    std::weak_ptr<Tensor> wptr;

    TensorWeakRef(const TensorWrapper& tw) : wptr(tw.m_tensor) {}

    py::object operator()() {
        if (auto p = wptr.lock()) {
            return TensorWrapper::make(p);
        }
        return py::none();
    }
};

/* ============== convert inputs ============== */

// map numpy.dtype.kind to priority
inline uint8_t category_priority(char c) {
    switch (c) {
        case 'f': return 3; // floating-point
        case 'i': return 2; // signed integer
        case 'u': return 2; // unsigned integer
        case 'b': return 1; // boolean
        default: return 0;
    }
}

// Returns the maximum value of the priority of each type in the list `types`.
uint8_t max_priority(SmallVector<PyArray_Descr*> types) {
    if (types.size() == 0) {
        return 0;
    } else {
        uint8_t max_p = 0;
        for (auto&& desc: types) {
            max_p = std::max(max_p, category_priority(desc->kind));
        }
        return max_p;
    }
}

// Returns the data type with sufficient size to hold all types of
// category `cat` in the list `types`.
PyArray_Descr* promote_types(SmallVector<PyArray_Descr*> types, uint8_t cat) {
    // Return value: New reference
    SmallVector<PyArray_Descr*> used_types;
    for (auto&& desc: types) {
        auto&& v = category_priority(desc->kind);
        if (v == cat) {
            used_types.emplace_back(desc);
        }
    }
    mgb_assert(used_types.size() > 0, "size of used_types is 0");
    PyArray_Descr* res = used_types[0];
    Py_INCREF(res);

    for (size_t i = 1; i < used_types.size(); ++i) {
        PyArray_Descr* tmp = PyArray_PromoteTypes(used_types[i], res);
        Py_DECREF(res);
        res = tmp;
    }
    return res;
}

PyArray_Descr* scalar2dtype(PyObject* arg) {
    // Return value: New reference
    if (PyBool_Check(arg)) {
        auto&& descr = PyArray_DescrFromType(NPY_BOOL);
        return descr;
    }
    if (PyLong_CheckExact(arg)) {
        auto&& descr = PyArray_DescrFromType(NPY_INT32);
        return descr;
    }
    if (PyFloat_CheckExact(arg)) {
        auto&& descr = PyArray_DescrFromType(NPY_FLOAT32);
        return descr;
    }
    return nullptr;
}

PyArray_Descr* _dtype_promotion(PyObject*const* args, size_t nargs) {
    // Return value: New reference
    SmallVector<PyArray_Descr*> tensors;
    SmallVector<PyArray_Descr*> scalars;

    bool is_tuple = false;
    PyObject* tuple;
    if (nargs == 1 && (PyTuple_Check(args[0]) || PyList_Check(args[0]))) {
        if (PyList_Check(args[0])) {
            tuple = PyList_AsTuple(args[0]);
        } else {
            tuple = args[0];
            Py_INCREF(tuple);
        }
        nargs = PyTuple_Size(tuple);
        is_tuple = true;
    }

    for (size_t i = 0; i < nargs; ++i) {
        PyObject* handle = is_tuple ? PyTuple_GetItem(tuple, i): args[i];
        if (handle == Py_None) continue;
        TensorWrapper* tw = TensorWrapper::try_cast(handle);
        if (tw) {
            mgb::DType type = tw->m_tensor->dtype();
            auto&& descr = npy::dtype_mgb2np_descr(type);
            Py_INCREF(descr.get());
            tensors.emplace_back(descr.get());
        }else{
            if (PyArray_Check(handle) || PyArray_CheckScalar(handle)) {
                auto&& descr = PyArray_DescrFromObject(handle, nullptr);
                tensors.emplace_back(descr);
                continue;
            }
            PyArray_Descr* descr = scalar2dtype(handle);
            if (descr) {
                scalars.emplace_back(descr);
                continue;
            }
        }
    }

    auto max_pri_scalars = max_priority(scalars);
    auto max_pri_tensors = max_priority(tensors);

    if (max_pri_scalars <= 0 && max_pri_tensors <= 0) {
        throw py::value_error("invalid input, no dtype avaliable");
    }
    PyArray_Descr* res;
    if (max_pri_scalars > max_pri_tensors) {
        res = promote_types(scalars, max_pri_scalars);
    }else{
        res = promote_types(tensors, max_pri_tensors);
    }
    for (auto *p: tensors) { Py_DECREF(p); }
    for (auto *p: scalars) { Py_DECREF(p); }
    Py_DECREF(tuple);
    return res;
}

CompNode _get_device(PyObject*const* args, size_t nargs) {
    bool is_tuple = false;
    PyObject* tuple;
    if (nargs == 1 && (PyTuple_Check(args[0]) || PyList_Check(args[0]))) {
        if (PyList_Check(args[0])) {
            tuple = PyList_AsTuple(args[0]);
        } else {
            tuple = args[0];
            Py_INCREF(tuple);
        }
        nargs = PyTuple_Size(tuple);
        is_tuple = true;
    }
    bool valid = false;
    CompNode cn;
    for (size_t i = 0; i < nargs; ++i) {
        PyObject* handle = is_tuple ? PyTuple_GetItem(tuple, i): args[i];
        TensorWrapper* tw = TensorWrapper::try_cast(handle);
        if (tw) {
            if (!valid) {
                cn = tw->m_tensor->comp_node();
                valid = true;
            } else {
                CompNode cn1 = tw->m_tensor->comp_node();
                if (cn1 != cn) {
                    throw py::value_error(ssprintf("ambiguous device: %s vs %s",
                        cn.to_string().c_str(), cn1.to_string().c_str()));
                }
            }
        }
    }
    if (!valid) {
        mgb_assert(0, "expect at least 1 device");
    }
    Py_DECREF(tuple);
    return cn;
}

// Returns the dtype that would result from performing an arithmetic
// operation on the provided input tensors and scalars.
PyObject* dtype_promotion(PyObject* self, PyObject*const* args, size_t nargs) {
    if (!nargs) {
        PyErr_SetString(PyExc_TypeError, "empty input is not allowed");
        return nullptr;
    }
    try {
        PyArray_Descr* res = _dtype_promotion(args, nargs);
        return py::cast(npy::dtype_np2mgb_descr(res)).release().ptr();
    } catch (std::exception& e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }
}

PyObject* get_device(PyObject* self, PyObject*const* args, size_t nargs) {
    if (!nargs) {
        PyErr_SetString(PyExc_TypeError, "empty input is not allowed");
        return nullptr;
    }
    try {
        CompNode cn = _get_device(args, nargs);
        return py::cast(cn).release().ptr();
    } catch (std::exception& e) {
        PyErr_SetString(PyExc_RuntimeError, e.what());
        return nullptr;
    }
}

#ifdef METH_FASTCALL
#define MGE_PY_INTERFACE(NAME, FUNC) \
    { #NAME, (PyCFunction)FUNC, METH_FASTCALL, nullptr }
#else
#define WRAP_FUNC_PY35(FUNC)                                \
    PyObject* py35_##FUNC(PyObject* self, PyObject* args) { \
        auto* arr = &PyTuple_GET_ITEM(args, 0);             \
        auto size = PyTuple_GET_SIZE(args);                 \
        return FUNC(self, arr, size);                       \
    }
WRAP_FUNC_PY35(py_apply);
WRAP_FUNC_PY35(dtype_promotion);
WRAP_FUNC_PY35(get_device);
#undef WRAP_FUNC_PY35
#define MGE_PY_INTERFACE(NAME, FUNC) \
    { #NAME, (PyCFunction)py35_##FUNC, METH_VARARGS, nullptr }
#endif

py::object make_empty_tensorwrapper() {
    return TensorWrapper::make(std::move(std::make_shared<Tensor>()));
}

void init_tensor(py::module m) {
    interpreter_for_py = interpreter::Interpreter::inst().create_channel();

    auto* tensor_type = TensorWrapper::wrap_t::type()
        .def<&TensorWrapper::numpy>("numpy")
        .def_getset<&TensorWrapper::shape>("shape")
        .def_getset<&TensorWrapper::dtype>("dtype")
        .def_getset<&TensorWrapper::device>("device")
        .def<&TensorWrapper::reset>("_reset")
        .def<&TensorWrapper::isscalar>("isscalar")
        .def<&TensorWrapper::setscalar>("setscalar")
        .def<&TensorWrapper::detach>("detach")
        .def<&TensorWrapper::_dev_tensor>("_dev_tensor")
        .def<&TensorWrapper::_swap_out>("_swap_out")
        .def<&TensorWrapper::_swap_in>("_swap_in")
        .def<&TensorWrapper::_drop>("_drop")
        .def<&TensorWrapper::reset_varnode>("_reset_varnode")
        .def_getset<&TensorWrapper::varnode>("_varnode")
        .def_getset<&TensorWrapper::data_read, &TensorWrapper::set_data_read>("data_read")
        .def_getset<&TensorWrapper::value_read, &TensorWrapper::set_value_read>("value_read")
        .def_getset<&TensorWrapper::shape_read, &TensorWrapper::set_shape_read>("shape_read")
        .def_getset<&TensorWrapper::mixin_handle, &TensorWrapper::set_mixin_handle>("mixin_handle")
        .def_getset<&TensorWrapper::handle, &TensorWrapper::set_handle>("_handle")
        .finalize();
    if (!tensor_type) throw py::error_already_set();
    py::setattr(m, "Tensor", tensor_type);

    py::class_<TensorWeakRef>(m, "TensorWeakRef")
        .def(py::init<const TensorWrapper&>())
        .def("__call__", &TensorWeakRef::operator());

    static PyMethodDef method_defs[] = {
            MGE_PY_INTERFACE(apply, py_apply),
            MGE_PY_INTERFACE(dtype_promotion, dtype_promotion),
            MGE_PY_INTERFACE(get_device, get_device),
            {nullptr, nullptr, 0, nullptr}};
    for (auto&& def: method_defs) {
        if (def.ml_meth != nullptr) {
            auto* func = PyCFunction_NewEx(&def, nullptr, nullptr);
            if (!func) throw py::error_already_set();
            py::setattr(m, def.ml_name, func);
        }
    }

    m.def("_set_swap_flag",
          [](bool flag) { interpreter_for_py->set_swap_flag(flag); });
    m.def("_set_drop_flag",
          [](bool flag) { interpreter_for_py->set_drop_flag(flag); });
    m.def("config_async_level",
          [](int level) { interpreter_for_py->config_async_level(level); });
    m.def("get_async_level",
          []() { return interpreter_for_py->get_async_level(); });
    m.def("sync",
          []() {
              interpreter_for_py->sync();
              py_task_q.wait_all_task_finish();
          },
          py::call_guard<py::gil_scoped_release>());
    m.def("full_sync",
          []() {
              interpreter_for_py->sync();
              CompNode::sync_all();
              py_task_q.wait_all_task_finish();
          },
          py::call_guard<py::gil_scoped_release>());

    py::handle grad_key_type = GradKeyWrapper::wrap_t::type()
        .def<&GradKeyWrapper::attach>("attach")
        .def<&GradKeyWrapper::is_attached_to>("is_attached_to")
        .def_getset<&GradKeyWrapper::get_name, &GradKeyWrapper::set_name>("name")
        .finalize();
    if (!grad_key_type) throw py::error_already_set();
    py::setattr(m, "GradKey", grad_key_type);
    m.def("backward", &GradKeyWrapper::backward);

    m.def("set_cpp_apply_with_tracing", &set_cpp_apply_with_tracing);
    m.def("set_cpp_apply_const_with_tracing", &set_cpp_apply_const_with_tracing);
    m.def("set_cpp_apply_compiled_mode", &set_cpp_apply_compiled_mode);
    m.def("set_cpp_apply_const_compiled_mode", &set_cpp_apply_const_compiled_mode);
    m.def("set_cpp_apply_backward_varnode", &set_cpp_apply_backward_varnode);

    m.attr("skip_tracing") = &skip_tracing;

    py::class_<SharedHandle>(m, "SharedHandle")
        .def(py::init<const SharedHandle&>());

    m.def("set_tracing", &set_tracing);
    m.def("unset_tracing", &unset_tracing);
    m.def("set_symbolic", &set_symbolic);
    m.def("unset_symbolic", &unset_symbolic);
    m.def("set_compiled", &set_compiled);
    m.def("unset_compiled", &unset_compiled);

    m.def("__make_empty_tensor", &make_empty_tensorwrapper);
}

#undef MGE_PY_INTERFACE

} // namespace mgb::imperative::python
