/*******************************************************************************
* Copyright 2017-2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "mkldnn_types.h"

#include "c_types_map.hpp"
#include "jit_uni_pooling.hpp"
#include "type_helpers.hpp"
#include "bfloat16_utils.hpp"
#include "nstl.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

template <cpu_isa_t isa, data_type_t d_type>
void jit_uni_pooling_fwd_t<isa, d_type>::execute_forward() const {
    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto dst = reinterpret_cast<data_t*>(this->memory(0));
    auto indices = pd()->desc()->alg_kind == alg_kind::pooling_max ?
        reinterpret_cast<unsigned char *>(this->memory(1)) : nullptr;

    const memory_desc_wrapper src_d(pd()->src_pd());
    const memory_desc_wrapper dst_d(pd()->dst_pd());
    const memory_desc_wrapper indices_d(pd()->workspace_pd());
    const size_t ind_dt_size = indices
        ? types::data_type_size(indices_d.data_type()) : 0;

    const auto &jpp = pd()->jpp_;
    int mb = pd()->MB();

    auto ker = [&](int n, int b_c, int oh) {
        auto arg = jit_pool_call_s();

        const int ij = oh * jpp.stride_h;
        const int i_t_overflow = nstl::max(0, jpp.t_pad-ij);
        const int i_b_overflow = nstl::max(jpp.ih, ij+jpp.kh-jpp.t_pad)-jpp.ih;
        const int ih = nstl::max(ij - jpp.t_pad, 0);

        arg.src = (const void*)&src[src_d.blk_off(n, b_c, ih)];
        arg.dst = (const void*)&dst[dst_d.blk_off(n, b_c, oh)];
        if (indices) {
            const size_t ind_off = indices_d.blk_off(n, b_c, oh);
            arg.indices = (const void*)&indices[ind_off * ind_dt_size];
        }
        arg.oh = oh == 0;
        arg.kh_padding = jpp.kh - i_t_overflow - i_b_overflow;
        arg.kh_padding_shift = i_t_overflow*jpp.kw;
        arg.kw_padding = 0;
        arg.ker_area_h = pd()->desc()->alg_kind == alg_kind::pooling_avg_exclude_padding
             ?  (float)(jpp.kh - nstl::max(0, oh*jpp.stride_h - jpp.t_pad + jpp.kh - jpp.ih) -
                nstl::max(0, jpp.t_pad - oh*jpp.stride_h))
             :  (float)(jpp.kh - nstl::max(0, oh*jpp.stride_h - jpp.t_pad + jpp.kh - jpp.ih - jpp.b_pad));

        (*kernel_)(&arg);
    };

    parallel_nd(mb, jpp.nb_c, jpp.oh,
        [&](int n, int b_c, int oh) {
        ker(n, b_c, oh);
    });
}

template <cpu_isa_t isa, data_type_t d_type>
void jit_uni_pooling_fwd_t<isa, d_type>::execute_forward_3d() const {
    auto src = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto dst = reinterpret_cast<data_t*>(this->memory(0));
    auto indices = pd()->desc()->alg_kind == alg_kind::pooling_max ?
        reinterpret_cast<unsigned char *>(this->memory(1)) : nullptr;

    const memory_desc_wrapper src_d(pd()->src_pd());
    const memory_desc_wrapper dst_d(pd()->dst_pd());
    const memory_desc_wrapper indices_d(pd()->workspace_pd());
    const size_t ind_dt_size = indices
        ? types::data_type_size(indices_d.data_type()) : 0;

    const auto &jpp = pd()->jpp_;
    int mb = pd()->MB();

    auto ker = [&](int n, int b_c, int od, int oh, int id, int d_t_overflow,
            int d_b_overflow) {
        auto arg = jit_pool_call_s();

        const int ij = oh * jpp.stride_h;
        const int i_t_overflow = nstl::max(0, jpp.t_pad-ij);
        const int i_b_overflow = nstl::max(jpp.ih, ij+jpp.kh-jpp.t_pad)-jpp.ih;
        const int ih = nstl::max(ij - jpp.t_pad, 0);

        arg.src = &src[src_d.blk_off(n, b_c, id, ih)];
        arg.dst = &dst[dst_d.blk_off(n, b_c, od, oh)];
        if (indices) {
            const size_t ind_off = indices_d.blk_off(n, b_c, od, oh);
            arg.indices = &indices[ind_off * ind_dt_size];
        }
        arg.oh = (oh + od == 0);
        arg.kd_padding = jpp.kd - d_t_overflow - d_b_overflow;
        arg.kh_padding = jpp.kh - i_t_overflow - i_b_overflow;
        arg.kh_padding_shift = i_t_overflow*jpp.kw + d_t_overflow*jpp.kw*jpp.kh;
        arg.kd_padding_shift = (i_t_overflow + i_b_overflow)*jpp.kw;
        arg.kw_padding = 0;
        arg.ker_area_h = (float)(jpp.kh -
            nstl::max(0, oh*jpp.stride_h - jpp.t_pad + jpp.kh - jpp.ih) -
            nstl::max(0, jpp.t_pad - oh*jpp.stride_h)) * (jpp.kd -
            nstl::max(0, od*jpp.stride_d - jpp.f_pad + jpp.kd - jpp.id) -
            nstl::max(0, jpp.f_pad - od*jpp.stride_d));


        (*kernel_)(&arg);
    };

    parallel_nd(mb, jpp.nb_c, jpp.od,
        [&](int n, int b_c, int od) {
        const int ik = od * jpp.stride_d;
        const int d_t_overflow = nstl::max(0, jpp.f_pad-ik);
        const int d_b_overflow = nstl::max(jpp.id, ik+jpp.kd-jpp.f_pad)
            -jpp.id;
        const int id = nstl::max(ik - jpp.f_pad, 0);
        for (int oh = 0; oh < jpp.oh; ++oh) {
            ker(n, b_c, od, oh, id, d_t_overflow, d_b_overflow);
        }
    });
}

template <cpu_isa_t isa, data_type_t d_type>
void jit_uni_pooling_bwd_t<isa, d_type>::execute_backward() const {
    auto diff_dst = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto diff_src = reinterpret_cast<data_t*>(this->memory(0));
    auto indices = pd()->desc()->alg_kind == alg_kind::pooling_max ?
        reinterpret_cast<const char*>(this->input_memory(1)) : nullptr;

    const memory_desc_wrapper diff_src_d(pd()->diff_src_pd());
    const memory_desc_wrapper diff_dst_d(pd()->diff_dst_pd());
    const memory_desc_wrapper indices_d(pd()->workspace_pd());
    const size_t ind_dt_size = indices
        ? types::data_type_size(indices_d.data_type()) : 0;

    const auto &jpp = pd()->jpp_;
    int mb = pd()->MB();

    auto ker = [&](int n, int b_c, int oh) {
        auto arg = jit_pool_call_s();

        const int ij = oh * jpp.stride_h;
        const int i_t_overflow = nstl::max(0, jpp.t_pad-ij);
        const int i_b_overflow = nstl::max(jpp.ih, ij+jpp.kh-jpp.t_pad)-jpp.ih;
        const int ih = nstl::max(ij - jpp.t_pad, 0);

        arg.src = &diff_src[diff_src_d.blk_off(n, b_c, ih)];
        arg.dst = &diff_dst[diff_dst_d.blk_off(n, b_c, oh)];
        if (indices) {
            const size_t ind_off = indices_d.blk_off(n, b_c, oh);
            arg.indices = &indices[ind_off * ind_dt_size];
        }
        arg.oh = (oh == 0);
        arg.kh_padding = jpp.kh - i_t_overflow - i_b_overflow;
        arg.kh_padding_shift = i_t_overflow*jpp.kw;
        arg.kw_padding = 0;
        arg.ker_area_h = (float)(jpp.kh -
            nstl::max(0, oh*jpp.stride_h - jpp.t_pad + jpp.kh - jpp.ih) -
            nstl::max(0, jpp.t_pad - oh*jpp.stride_h));

        (*kernel_)(&arg);
    };

    parallel_nd(mb, jpp.nb_c, [&](int n, int b_c) {
        for (int oh = 0; oh < jpp.oh; ++oh) {
            ker(n, b_c, oh);
        }
    });
}

template <cpu_isa_t isa, data_type_t d_type>
void jit_uni_pooling_bwd_t<isa, d_type>::execute_backward_3d() const {
    auto diff_dst = reinterpret_cast<const data_t *>(this->input_memory(0));
    auto diff_src = reinterpret_cast<data_t*>(this->memory(0));
    auto indices = pd()->desc()->alg_kind == alg_kind::pooling_max ?
        reinterpret_cast<const char*>(this->input_memory(1)) : nullptr;

    const memory_desc_wrapper diff_src_d(pd()->diff_src_pd());
    const memory_desc_wrapper diff_dst_d(pd()->diff_dst_pd());
    const memory_desc_wrapper indices_d(pd()->workspace_pd());
    const size_t ind_dt_size = indices
        ? types::data_type_size(indices_d.data_type()) : 0;

    const auto &jpp = pd()->jpp_;
    int mb = pd()->MB();

    auto ker = [&](int n, int b_c, int od, int oh, int id, int d_t_overflow,
            int d_b_overflow, int zero_size, int kd) {
        auto arg = jit_pool_call_s();

        const int ij = oh * jpp.stride_h;
        const int i_t_overflow = nstl::max(0, jpp.t_pad-ij);
        const int i_b_overflow = nstl::max(jpp.ih, ij+jpp.kh-jpp.t_pad)-jpp.ih;
        const int ih = nstl::max(ij - jpp.t_pad, 0);

        arg.src = (const void*)&diff_src[diff_src_d.blk_off(n, b_c, id + kd, ih)];
        arg.dst = (const void*)&diff_dst[diff_dst_d.blk_off(n, b_c, od, oh)];
        if (indices) {
            const size_t ind_off = indices_d.blk_off(n, b_c, od, oh);
            arg.indices = (const void*)&indices[ind_off * ind_dt_size];
        }
        arg.oh = zero_size;
        arg.kd_padding = jpp.kd - d_t_overflow - d_b_overflow;
        arg.kh_padding = jpp.kh - i_t_overflow - i_b_overflow;
        arg.kh_padding_shift = i_t_overflow*jpp.kw + d_t_overflow*jpp.kw*jpp.kh
            + kd * jpp.kw * jpp.kh;
        arg.kd_padding_shift = (i_t_overflow + i_b_overflow)*jpp.kw;
        arg.kw_padding = 0;
        arg.ker_area_h = (float)(jpp.kh -
            nstl::max(0, oh*jpp.stride_h - jpp.t_pad + jpp.kh - jpp.ih) -
            nstl::max(0, jpp.t_pad - oh*jpp.stride_h)) * (jpp.kd -
            nstl::max(0, od*jpp.stride_d - jpp.f_pad + jpp.kd - jpp.id) -
            nstl::max(0, jpp.f_pad - od*jpp.stride_d));

        (*kernel_)(&arg);
    };

    if (jpp.simple_alg) {
        const int neg_back_pad = -(jpp.od - 1) * jpp.stride_d - jpp.kd
            + jpp.f_pad + jpp.id;

        parallel_nd(mb, jpp.nb_c, jpp.od,
            [&](int n, int b_c, int od) {
            const int ik = od * jpp.stride_d;
            const int d_t_overflow = nstl::max(0, jpp.f_pad - ik);
            const int d_b_overflow = nstl::max(jpp.id, ik + jpp.kd
                    - jpp.f_pad) - jpp.id;
            const int id = nstl::max(ik - jpp.f_pad, 0);
            int zero_s = jpp.stride_d - d_t_overflow - (nstl::max(
                    jpp.id, ik + jpp.stride_d - jpp.f_pad) - jpp.id);
            for (int oh = 0; oh < jpp.oh; ++oh) {
                ker(n, b_c, od, oh, id, d_t_overflow, d_b_overflow,
                        (oh == 0) ? zero_s : 0, 0);
            }

            // zero-out untouched portion of diff_src when back_pad is negative
            if (neg_back_pad > 0 && od == jpp.od - 1) {
                auto blk_start_ptr = &diff_src[diff_src_d.blk_off(n, b_c,
                    jpp.id - neg_back_pad, 0, 0)];
                auto blk_size = neg_back_pad * jpp.ih * jpp.iw;

                for (auto blk_idx = 0; blk_idx < blk_size; ++blk_idx) {
                    auto blk_ptr = blk_start_ptr + blk_idx * jpp.c_block;

                    PRAGMA_OMP_SIMD()
                    for (auto ch_idx = 0; ch_idx < jpp.c_block; ++ch_idx)
                        blk_ptr[ch_idx] = static_cast<data_t>(0.f);
                }
            }

        });
    } else {
        ptrdiff_t nelems = (ptrdiff_t)mb * (ptrdiff_t)jpp.c
            * (ptrdiff_t)jpp.id * (ptrdiff_t)jpp.ih * (ptrdiff_t)jpp.iw;

        if (diff_src_d.data_type() == data_type::bf16) {
            mkldnn_bfloat16_t bf16_zero =
                bf16_cvt_utils::cvt_float_to_bfloat16(0.f);
            parallel_nd(nelems, [&](ptrdiff_t i) {
                    diff_src[i] = bf16_zero;});
        } else
            parallel_nd(nelems, [&](ptrdiff_t i) {
                    diff_src[i] = static_cast<data_t>(0.f); });

        for (int kd = 0; kd < jpp.kd; ++kd) {
            parallel_nd(mb, jpp.nb_c, [&](int n, int b_c) {
                for (int od = 0; od < jpp.od; ++od) {
                    const int ik = od * jpp.stride_d;
                    const int d_t_overflow = nstl::max(0, jpp.f_pad-ik);
                    const int d_b_overflow = nstl::max(jpp.id, ik + jpp.kd
                            - jpp.f_pad) - jpp.id;
                    if (kd >= jpp.kd - d_t_overflow - d_b_overflow)
                        continue;
                    const int id = nstl::max(ik - jpp.f_pad, 0);
                    for (int oh = 0; oh < jpp.oh; ++oh) {
                        ker(n, b_c, od, oh, id, d_t_overflow, d_b_overflow,
                                0, kd);
                    }
                }
            });
        }
    }
}


template struct jit_uni_pooling_fwd_t<sse42, data_type::f32>;
template struct jit_uni_pooling_bwd_t<sse42, data_type::f32>;
template struct jit_uni_pooling_fwd_t<avx, data_type::f32>;
template struct jit_uni_pooling_bwd_t<avx, data_type::f32>;
template struct jit_uni_pooling_fwd_t<avx512_common, data_type::f32>;
template struct jit_uni_pooling_bwd_t<avx512_common, data_type::f32>;
template struct jit_uni_pooling_fwd_t<avx512_common, data_type::bf16>;
template struct jit_uni_pooling_bwd_t<avx512_common, data_type::bf16>;

}
}
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
