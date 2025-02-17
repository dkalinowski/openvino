// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "transformations/common_optimizations/space_to_batch_fusion.hpp"

#include <memory>
#include <ngraph/opsets/opset6.hpp>
#include <ngraph/pattern/op/or.hpp>
#include <ngraph/pattern/op/wrap_type.hpp>
#include <ngraph/rt_info.hpp>
#include <vector>

#include "itt.hpp"
#include "transformations/utils/utils.hpp"

ngraph::pass::SpaceToBatchFusion::SpaceToBatchFusion() {
    MATCHER_SCOPE(SpaceToBatchFusion);
    auto data_pattern = pattern::any_input();
    auto reshape_before_pattern =
        pattern::wrap_type<opset6::Reshape>({data_pattern, pattern::wrap_type<opset6::Constant>()},
                                            pattern::rank_equals(4));
    auto trans_before_pattern =
        pattern::wrap_type<opset6::Transpose>({data_pattern, pattern::wrap_type<opset6::Constant>()},
                                              pattern::rank_equals(4));
    auto reshape_or_transpose_before_pattern =
        std::make_shared<pattern::op::Or>(OutputVector{reshape_before_pattern, trans_before_pattern});
    auto pads_begin_pattern = pattern::wrap_type<opset6::Constant>();
    auto pads_end_pattern = pattern::wrap_type<opset6::Constant>();
    auto pad_value = pattern::wrap_type<opset6::Constant>();
    auto pad_pattern = pattern::wrap_type<opset6::Pad>(
        {reshape_or_transpose_before_pattern, pads_begin_pattern, pads_end_pattern, pad_value});
    auto space_to_depth_pattern = pattern::wrap_type<opset6::SpaceToDepth>({pad_pattern}, pattern::has_static_shape());
    auto reshape_after_pattern =
        pattern::wrap_type<opset6::Reshape>({space_to_depth_pattern, pattern::wrap_type<opset6::Constant>()},
                                            pattern::rank_equals(4));
    auto trans_after_pattern =
        pattern::wrap_type<opset6::Transpose>({space_to_depth_pattern, pattern::wrap_type<opset6::Constant>()},
                                              pattern::rank_equals(4));
    auto reshape_or_transpose_after_pattern =
        std::make_shared<pattern::op::Or>(OutputVector{reshape_after_pattern, trans_after_pattern});

    matcher_pass_callback callback = [=](pattern::Matcher& m) {
        const auto& pattern_map = m.get_pattern_value_map();

        auto get_reshape_or_transpose = [&pattern_map](
                                            const std::shared_ptr<Node>& reshape_pattern,
                                            const std::shared_ptr<Node>& trans_pattern) -> std::shared_ptr<Node> {
            if (pattern_map.count(reshape_pattern))
                return pattern_map.at(reshape_pattern).get_node_shared_ptr();
            if (pattern_map.count(trans_pattern))
                return pattern_map.at(trans_pattern).get_node_shared_ptr();
            return nullptr;
        };
        auto check_input_output_shape = [](const std::shared_ptr<Node>& node) -> bool {
            const auto& input_shape = node->get_input_shape(0);
            const auto& output_shape = node->get_output_shape(0);
            // Transpose permutation has to be [1, 0, 2, 3]
            return input_shape[0] == output_shape[1] && input_shape[1] == output_shape[0] &&
                   input_shape[2] == output_shape[2] && input_shape[3] == output_shape[3];
        };

        std::shared_ptr<Node> reshape_or_trans_before =
            get_reshape_or_transpose(reshape_before_pattern, trans_before_pattern);
        if (!reshape_or_trans_before)
            return false;
        std::shared_ptr<Node> reshape_or_trans_after =
            get_reshape_or_transpose(reshape_after_pattern, trans_after_pattern);
        if (!reshape_or_trans_after)
            return false;
        if (!check_input_output_shape(reshape_or_trans_before))
            return false;
        if (!check_input_output_shape(reshape_or_trans_after))
            return false;

        auto pad = std::dynamic_pointer_cast<opset6::Pad>(pattern_map.at(pad_pattern).get_node_shared_ptr());
        if (!pad || pad->get_pad_mode() != op::PadMode::CONSTANT)
            return false;
        auto pad_value_const =
            std::dynamic_pointer_cast<opset6::Constant>(pattern_map.at(pad_value).get_node_shared_ptr());
        if (!pad_value_const)
            return false;
        auto pad_value = pad_value_const->cast_vector<float>();
        if (pad_value.size() != 1 || pad_value[0] != 0.0f)
            return false;

        auto space_to_depth = std::dynamic_pointer_cast<opset6::SpaceToDepth>(
            pattern_map.at(space_to_depth_pattern).get_node_shared_ptr());
        if (!space_to_depth)
            return false;
        if (space_to_depth->get_mode() != opset6::SpaceToDepth::SpaceToDepthMode::BLOCKS_FIRST)
            return false;
        auto block_size = static_cast<int64_t>(space_to_depth->get_block_size());
        auto block_shape =
            op::Constant::create(element::i64, Shape{4}, std::vector<int64_t>{1, 1, block_size, block_size});
        auto space_to_batch = register_new_node<opset6::SpaceToBatch>(pattern_map.at(data_pattern),
                                                                      block_shape,
                                                                      pattern_map.at(pads_begin_pattern),
                                                                      pattern_map.at(pads_end_pattern));
        space_to_batch->set_friendly_name(reshape_or_trans_after->get_friendly_name());

        copy_runtime_info(
            {
                reshape_or_trans_before,
                pad,
                space_to_depth,
                reshape_or_trans_after,
            },
            space_to_batch);
        replace_node(reshape_or_trans_after, space_to_batch);

        return true;
    };

    auto m = std::make_shared<pattern::Matcher>(reshape_or_transpose_after_pattern, matcher_name);
    this->register_matcher(m, callback);
}
