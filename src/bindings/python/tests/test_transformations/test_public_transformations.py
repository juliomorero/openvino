# -*- coding: utf-8 -*-
# Copyright (C) 2022 Intel Corporation
# SPDX-License-Identifier: Apache-2.0
import os
import pytest
import numpy as np
import openvino.runtime as ov

from openvino.runtime import Model, PartialShape, Shape, opset8, Core
from openvino.runtime.passes import (
    Manager,
    ConstantFolding,
    MakeStateful,
    ConvertFP32ToFP16,
    LowLatency2,
    Serialize,
)
from tests.test_transformations.utils.utils import count_ops, get_test_model
from tests.test_utils.test_utils import create_filename_for_test


def get_model():
    param = opset8.parameter(PartialShape([1, 3, 22, 22]), name="parameter")
    param.get_output_tensor(0).set_names({"parameter"})
    relu = opset8.relu(param)
    reshape = opset8.reshape(relu, opset8.shape_of(relu), False)
    res = opset8.result(reshape, name="result")
    res.get_output_tensor(0).set_names({"result"})
    return Model([res], [param], "test")


def test_make_stateful():
    model = get_model()

    manager = Manager()
    model_pass = MakeStateful({"parameter": "result"})
    manager.register_pass(model_pass)
    manager.run_passes(model)

    assert model is not None
    assert len(model.get_parameters()) == 0
    assert len(model.get_results()) == 0


def test_constant_folding():
    model = get_model()

    manager = Manager()
    manager.register_pass(ConstantFolding())
    manager.run_passes(model)

    assert model is not None
    assert count_ops(model, "ShapeOf") == [0]


def test_convert_precision():
    model = get_model()
    param_dtype = model.get_parameters()[0].get_element_type().to_dtype()
    assert param_dtype == np.float32

    manager = Manager()
    manager.register_pass(ConvertFP32ToFP16())
    manager.run_passes(model)

    assert model is not None
    param_dtype = model.get_parameters()[0].get_element_type().to_dtype()
    assert param_dtype == np.float16


def test_low_latency2():
    param_x = opset8.parameter(Shape([32, 40, 10]), np.float32, "X")
    param_y = opset8.parameter(Shape([32, 40, 10]), np.float32, "Y")
    param_m = opset8.parameter(Shape([32, 2, 10]), np.float32, "M")

    x_i = opset8.parameter(Shape([32, 2, 10]), np.float32, "X_i")
    y_i = opset8.parameter(Shape([32, 2, 10]), np.float32, "Y_i")
    m_body = opset8.parameter(Shape([32, 2, 10]), np.float32, "M_body")

    add = opset8.add(x_i, y_i)
    zo = opset8.multiply(add, m_body)

    body = Model([zo], [x_i, y_i, m_body], "body_function")

    ti = opset8.tensor_iterator()
    ti.set_body(body)
    ti.set_sliced_input(x_i, param_x.output(0), 0, 2, 2, 39, 1)
    ti.set_sliced_input(y_i, param_y.output(0), 0, 2, 2, -1, 1)
    ti.set_invariant_input(m_body, param_m.output(0))

    out0 = ti.get_iter_value(zo.output(0), -1)
    out1 = ti.get_concatenated_slices(zo.output(0), 0, 2, 2, 39, 1)

    result0 = opset8.result(out0)
    result1 = opset8.result(out1)

    model = Model([result0, result1], [param_x, param_y, param_m])

    manager = Manager()
    manager.register_pass(LowLatency2())
    manager.run_passes(model)

    # TODO: create TI which will be transformed by LowLatency2
    assert count_ops(model, "TensorIterator") == [1]


# request - https://docs.pytest.org/en/7.1.x/reference/reference.html#request
@pytest.mark.parametrize("is_path_xml, is_path_bin", [  # noqa: PT006
    (True, True),
    (True, False),
    (False, True),
    (False, False),
],
)
def test_serialize_pass(request, is_path_xml, is_path_bin):
    core = Core()
    xml_path, bin_path = create_filename_for_test(request.node.name,
                                                  is_path_xml,
                                                  is_path_bin)

    func = get_test_model()

    manager = Manager()
    manager.register_pass(Serialize(xml_path, bin_path))
    manager.run_passes(func)

    assert func is not None

    res_func = core.read_model(model=xml_path, weights=bin_path)

    assert func.get_parameters() == res_func.get_parameters()
    assert func.get_ordered_ops() == res_func.get_ordered_ops()

    os.remove(xml_path)
    os.remove(bin_path)
