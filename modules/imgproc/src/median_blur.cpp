/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                           License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2000-2008, 2018, Intel Corporation, all rights reserved.
// Copyright (C) 2009, Willow Garage Inc., all rights reserved.
// Copyright (C) 2014-2015, Itseez Inc., all rights reserved.
// Third party copyrights are property of their respective owners.
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors "as is" and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include "precomp.hpp"

#include <vector>

#include "opencv2/core/hal/intrin.hpp"
#include "opencl_kernels_imgproc.hpp"

#include "opencv2/core/openvx/ovx_defs.hpp"

/*
 * This file includes the code, contributed by Simon Perreault
 * (the function icvMedianBlur_8u_O1)
 *
 * Constant-time median filtering -- http://nomis80.org/ctmf.html
 * Copyright (C) 2006 Simon Perreault
 *
 * Contact:
 *  Laboratoire de vision et systemes numeriques
 *  Pavillon Adrien-Pouliot
 *  Universite Laval
 *  Sainte-Foy, Quebec, Canada
 *  G1K 7P4
 *
 *  perreaul@gel.ulaval.ca
 */

/****************************************************************************************\
                                      Median Filter
\****************************************************************************************/

namespace cv
{

static void
medianBlur_8u_O1( const Mat& _src, Mat& _dst, int ksize )
{
    typedef ushort HT;

    /**
     * This structure represents a two-tier histogram. The first tier (known as the
     * "coarse" level) is 4 bit wide and the second tier (known as the "fine" level)
     * is 8 bit wide. Pixels inserted in the fine level also get inserted into the
     * coarse bucket designated by the 4 MSBs of the fine bucket value.
     *
     * The structure is aligned on 16 bits, which is a prerequisite for SIMD
     * instructions. Each bucket is 16 bit wide, which means that extra care must be
     * taken to prevent overflow.
     */
    typedef struct
    {
        HT coarse[16];
        HT fine[16][16];
    } Histogram;

/**
 * HOP is short for Histogram OPeration. This macro makes an operation \a op on
 * histogram \a h for pixel value \a x. It takes care of handling both levels.
 */
#define HOP(h,x,op) \
    h.coarse[x>>4] op, \
    *((HT*)h.fine + x) op

#define COP(c,j,x,op) \
    h_coarse[ 16*(n*c+j) + (x>>4) ] op, \
    h_fine[ 16 * (n*(16*c+(x>>4)) + j) + (x & 0xF) ] op

    int cn = _dst.channels(), m = _dst.rows, r = (ksize-1)/2;
    CV_Assert(cn > 0 && cn <= 4);
    size_t sstep = _src.step, dstep = _dst.step;
    Histogram CV_DECL_ALIGNED(16) H[4];
    HT CV_DECL_ALIGNED(16) luc[4][16];

    int STRIPE_SIZE = std::min( _dst.cols, 512/cn );

    std::vector<HT> _h_coarse(1 * 16 * (STRIPE_SIZE + 2*r) * cn + 16);
    std::vector<HT> _h_fine(16 * 16 * (STRIPE_SIZE + 2*r) * cn + 16);
    HT* h_coarse = alignPtr(&_h_coarse[0], 16);
    HT* h_fine = alignPtr(&_h_fine[0], 16);

    for( int x = 0; x < _dst.cols; x += STRIPE_SIZE )
    {
        int i, j, k, c, n = std::min(_dst.cols - x, STRIPE_SIZE) + r*2;
        const uchar* src = _src.ptr() + x*cn;
        uchar* dst = _dst.ptr() + (x - r)*cn;

        memset( h_coarse, 0, 16*n*cn*sizeof(h_coarse[0]) );
        memset( h_fine, 0, 16*16*n*cn*sizeof(h_fine[0]) );

        // First row initialization
        for( c = 0; c < cn; c++ )
        {
            for( j = 0; j < n; j++ )
                COP( c, j, src[cn*j+c], += (HT)(r+2) );

            for( i = 1; i < r; i++ )
            {
                const uchar* p = src + sstep*std::min(i, m-1);
                for ( j = 0; j < n; j++ )
                    COP( c, j, p[cn*j+c], ++ );
            }
        }

        for( i = 0; i < m; i++ )
        {
            const uchar* p0 = src + sstep * std::max( 0, i-r-1 );
            const uchar* p1 = src + sstep * std::min( m-1, i+r );

            memset( H, 0, cn*sizeof(H[0]) );
            memset( luc, 0, cn*sizeof(luc[0]) );
            for( c = 0; c < cn; c++ )
            {
                // Update column histograms for the entire row.
                for( j = 0; j < n; j++ )
                {
                    COP( c, j, p0[j*cn + c], -- );
                    COP( c, j, p1[j*cn + c], ++ );
                }

                // First column initialization
                for (k = 0; k < 16; ++k)
                {
#if CV_SIMD256
                    v_store(H[c].fine[k], v_mul_wrap(v256_load(h_fine + 16 * n*(16 * c + k)), v256_setall_u16(2 * r + 1)) + v256_load(H[c].fine[k]));
#elif CV_SIMD128
                    v_store(H[c].fine[k], v_mul_wrap(v_load(h_fine + 16 * n*(16 * c + k)), v_setall_u16((ushort)(2 * r + 1))) + v_load(H[c].fine[k]));
                    v_store(H[c].fine[k] + 8, v_mul_wrap(v_load(h_fine + 16 * n*(16 * c + k) + 8), v_setall_u16((ushort)(2 * r + 1))) + v_load(H[c].fine[k] + 8));
#else
                    for (int ind = 0; ind < 16; ++ind)
                        H[c].fine[k][ind] = (HT)(H[c].fine[k][ind] + (2 * r + 1) * h_fine[16 * n*(16 * c + k) + ind]);
#endif
                }

#if CV_SIMD256
                v_uint16x16 v_coarse = v256_load(H[c].coarse);
#elif CV_SIMD128
                v_uint16x8 v_coarsel = v_load(H[c].coarse);
                v_uint16x8 v_coarseh = v_load(H[c].coarse + 8);
#endif
                HT* px = h_coarse + 16 * n*c;
                for( j = 0; j < 2*r; ++j, px += 16 )
                {
#if CV_SIMD256
                    v_coarse += v256_load(px);
#elif CV_SIMD128
                    v_coarsel += v_load(px);
                    v_coarseh += v_load(px + 8);
#else
                    for (int ind = 0; ind < 16; ++ind)
                        H[c].coarse[ind] += px[ind];
#endif
                }

                for( j = r; j < n-r; j++ )
                {
                    int t = 2*r*r + 2*r, b, sum = 0;
                    HT* segment;

                    px = h_coarse + 16 * (n*c + std::min(j + r, n - 1));
#if CV_SIMD256
                    v_coarse += v256_load(px);
                    v_store(H[c].coarse, v_coarse);
#elif CV_SIMD128
                    v_coarsel += v_load(px);
                    v_coarseh += v_load(px + 8);
                    v_store(H[c].coarse, v_coarsel);
                    v_store(H[c].coarse + 8, v_coarseh);
#else
                    for (int ind = 0; ind < 16; ++ind)
                        H[c].coarse[ind] += px[ind];
#endif

                    // Find median at coarse level
                    for ( k = 0; k < 16 ; ++k )
                    {
                        sum += H[c].coarse[k];
                        if ( sum > t )
                        {
                            sum -= H[c].coarse[k];
                            break;
                        }
                    }
                    CV_Assert( k < 16 );

                    /* Update corresponding histogram segment */
#if CV_SIMD256
                    v_uint16x16 v_fine;
#elif CV_SIMD128
                    v_uint16x8 v_finel;
                    v_uint16x8 v_fineh;
#endif
                    if ( luc[c][k] <= j-r )
                    {
#if CV_SIMD256
                        v_fine = v256_setzero_u16();
#elif CV_SIMD128
                        v_finel = v_setzero_u16();
                        v_fineh = v_setzero_u16();
#else
                        memset(&H[c].fine[k], 0, 16 * sizeof(HT));
#endif
                        px = h_fine + 16 * (n*(16 * c + k) + j - r);
                        for (luc[c][k] = HT(j - r); luc[c][k] < MIN(j + r + 1, n); ++luc[c][k], px += 16)
                        {
#if CV_SIMD256
                            v_fine += v256_load(px);
#elif CV_SIMD128
                            v_finel += v_load(px);
                            v_fineh += v_load(px + 8);
#else
                            for (int ind = 0; ind < 16; ++ind)
                                H[c].fine[k][ind] += px[ind];
#endif
                        }

                        if ( luc[c][k] < j+r+1 )
                        {
                            px = h_fine + 16 * (n*(16 * c + k) + (n - 1));
#if CV_SIMD256
                            v_fine += v_mul_wrap(v256_load(px), v256_setall_u16(j + r + 1 - n));
#elif CV_SIMD128
                            v_finel += v_mul_wrap(v_load(px), v_setall_u16((ushort)(j + r + 1 - n)));
                            v_fineh += v_mul_wrap(v_load(px + 8), v_setall_u16((ushort)(j + r + 1 - n)));
#else
                            for (int ind = 0; ind < 16; ++ind)
                                H[c].fine[k][ind] = (HT)(H[c].fine[k][ind] + (j + r + 1 - n) * px[ind]);
#endif
                            luc[c][k] = (HT)(j+r+1);
                        }
                    }
                    else
                    {
#if CV_SIMD256
                        v_fine = v256_load(H[c].fine[k]);
#elif CV_SIMD128
                        v_finel = v_load(H[c].fine[k]);
                        v_fineh = v_load(H[c].fine[k] + 8);
#endif
                        px = h_fine + 16*n*(16 * c + k);
                        for ( ; luc[c][k] < j+r+1; ++luc[c][k] )
                        {
#if CV_SIMD256
                            v_fine += v256_load(px + 16 * MIN(luc[c][k], n - 1)) - v256_load(px + 16 * MAX(luc[c][k] - 2 * r - 1, 0));
#elif CV_SIMD128
                            v_finel += v_load(px + 16 * MIN(luc[c][k], n - 1)    ) - v_load(px + 16 * MAX(luc[c][k] - 2 * r - 1, 0));
                            v_fineh += v_load(px + 16 * MIN(luc[c][k], n - 1) + 8) - v_load(px + 16 * MAX(luc[c][k] - 2 * r - 1, 0) + 8);
#else
                            for (int ind = 0; ind < 16; ++ind)
                                H[c].fine[k][ind] += px[16 * MIN(luc[c][k], n - 1) + ind] - px[16 * MAX(luc[c][k] - 2 * r - 1, 0) + ind];
#endif
                        }
                    }

                    px = h_coarse + 16 * (n*c + MAX(j - r, 0));
#if CV_SIMD256
                    v_store(H[c].fine[k], v_fine);
                    v_coarse -= v256_load(px);
#elif CV_SIMD128
                    v_store(H[c].fine[k], v_finel);
                    v_store(H[c].fine[k] + 8, v_fineh);
                    v_coarsel -= v_load(px);
                    v_coarseh -= v_load(px + 8);
#else
                    for (int ind = 0; ind < 16; ++ind)
                        H[c].coarse[ind] -= px[ind];
#endif

                    /* Find median in segment */
                    segment = H[c].fine[k];
                    for ( b = 0; b < 16 ; b++ )
                    {
                        sum += segment[b];
                        if ( sum > t )
                        {
                            dst[dstep*i+cn*j+c] = (uchar)(16*k + b);
                            break;
                        }
                    }
                    CV_Assert( b < 16 );
                }
            }
#if CV_SIMD
            vx_cleanup();
#endif
        }
    }

#undef HOP
#undef COP
}

static void
medianBlur_8u_Om( const Mat& _src, Mat& _dst, int m )
{
    #define N  16
    int     zone0[4][N];
    int     zone1[4][N*N];
    int     x, y;
    int     n2 = m*m/2;
    Size    size = _dst.size();
    const uchar* src = _src.ptr();
    uchar*  dst = _dst.ptr();
    int     src_step = (int)_src.step, dst_step = (int)_dst.step;
    int     cn = _src.channels();
    const uchar*  src_max = src + size.height*src_step;
    CV_Assert(cn > 0 && cn <= 4);

    #define UPDATE_ACC01( pix, cn, op ) \
    {                                   \
        int p = (pix);                  \
        zone1[cn][p] op;                \
        zone0[cn][p >> 4] op;           \
    }

    //CV_Assert( size.height >= nx && size.width >= nx );
    for( x = 0; x < size.width; x++, src += cn, dst += cn )
    {
        uchar* dst_cur = dst;
        const uchar* src_top = src;
        const uchar* src_bottom = src;
        int k, c;
        int src_step1 = src_step, dst_step1 = dst_step;

        if( x % 2 != 0 )
        {
            src_bottom = src_top += src_step*(size.height-1);
            dst_cur += dst_step*(size.height-1);
            src_step1 = -src_step1;
            dst_step1 = -dst_step1;
        }

        // init accumulator
        memset( zone0, 0, sizeof(zone0[0])*cn );
        memset( zone1, 0, sizeof(zone1[0])*cn );

        for( y = 0; y <= m/2; y++ )
        {
            for( c = 0; c < cn; c++ )
            {
                if( y > 0 )
                {
                    for( k = 0; k < m*cn; k += cn )
                        UPDATE_ACC01( src_bottom[k+c], c, ++ );
                }
                else
                {
                    for( k = 0; k < m*cn; k += cn )
                        UPDATE_ACC01( src_bottom[k+c], c, += m/2+1 );
                }
            }

            if( (src_step1 > 0 && y < size.height-1) ||
                (src_step1 < 0 && size.height-y-1 > 0) )
                src_bottom += src_step1;
        }

        for( y = 0; y < size.height; y++, dst_cur += dst_step1 )
        {
            // find median
            for( c = 0; c < cn; c++ )
            {
                int s = 0;
                for( k = 0; ; k++ )
                {
                    int t = s + zone0[c][k];
                    if( t > n2 ) break;
                    s = t;
                }

                for( k *= N; ;k++ )
                {
                    s += zone1[c][k];
                    if( s > n2 ) break;
                }

                dst_cur[c] = (uchar)k;
            }

            if( y+1 == size.height )
                break;

            if( cn == 1 )
            {
                for( k = 0; k < m; k++ )
                {
                    int p = src_top[k];
                    int q = src_bottom[k];
                    zone1[0][p]--;
                    zone0[0][p>>4]--;
                    zone1[0][q]++;
                    zone0[0][q>>4]++;
                }
            }
            else if( cn == 3 )
            {
                for( k = 0; k < m*3; k += 3 )
                {
                    UPDATE_ACC01( src_top[k], 0, -- );
                    UPDATE_ACC01( src_top[k+1], 1, -- );
                    UPDATE_ACC01( src_top[k+2], 2, -- );

                    UPDATE_ACC01( src_bottom[k], 0, ++ );
                    UPDATE_ACC01( src_bottom[k+1], 1, ++ );
                    UPDATE_ACC01( src_bottom[k+2], 2, ++ );
                }
            }
            else
            {
                assert( cn == 4 );
                for( k = 0; k < m*4; k += 4 )
                {
                    UPDATE_ACC01( src_top[k], 0, -- );
                    UPDATE_ACC01( src_top[k+1], 1, -- );
                    UPDATE_ACC01( src_top[k+2], 2, -- );
                    UPDATE_ACC01( src_top[k+3], 3, -- );

                    UPDATE_ACC01( src_bottom[k], 0, ++ );
                    UPDATE_ACC01( src_bottom[k+1], 1, ++ );
                    UPDATE_ACC01( src_bottom[k+2], 2, ++ );
                    UPDATE_ACC01( src_bottom[k+3], 3, ++ );
                }
            }

            if( (src_step1 > 0 && src_bottom + src_step1 < src_max) ||
                (src_step1 < 0 && src_bottom + src_step1 >= src) )
                src_bottom += src_step1;

            if( y >= m/2 )
                src_top += src_step1;
        }
    }
#undef N
#undef UPDATE_ACC
}


namespace {

struct MinMax8u
{
    typedef uchar value_type;
    typedef int arg_type;
    enum { SIZE = 1 };
    arg_type load(const uchar* ptr) { return *ptr; }
    void store(uchar* ptr, arg_type val) { *ptr = (uchar)val; }
    void operator()(arg_type& a, arg_type& b) const
    {
        int t = CV_FAST_CAST_8U(a - b);
        b += t; a -= t;
    }
};

struct MinMax16u
{
    typedef ushort value_type;
    typedef int arg_type;
    enum { SIZE = 1 };
    arg_type load(const ushort* ptr) { return *ptr; }
    void store(ushort* ptr, arg_type val) { *ptr = (ushort)val; }
    void operator()(arg_type& a, arg_type& b) const
    {
        arg_type t = a;
        a = std::min(a, b);
        b = std::max(b, t);
    }
};

struct MinMax16s
{
    typedef short value_type;
    typedef int arg_type;
    enum { SIZE = 1 };
    arg_type load(const short* ptr) { return *ptr; }
    void store(short* ptr, arg_type val) { *ptr = (short)val; }
    void operator()(arg_type& a, arg_type& b) const
    {
        arg_type t = a;
        a = std::min(a, b);
        b = std::max(b, t);
    }
};

struct MinMax32f
{
    typedef float value_type;
    typedef float arg_type;
    enum { SIZE = 1 };
    arg_type load(const float* ptr) { return *ptr; }
    void store(float* ptr, arg_type val) { *ptr = val; }
    void operator()(arg_type& a, arg_type& b) const
    {
        arg_type t = a;
        a = std::min(a, b);
        b = std::max(b, t);
    }
};

#if CV_SIMD

struct MinMaxVec8u
{
    typedef uchar value_type;
    typedef v_uint8x16 arg_type;
    enum { SIZE = v_uint8x16::nlanes };
    arg_type load(const uchar* ptr) { return v_load(ptr); }
    void store(uchar* ptr, const arg_type &val) { v_store(ptr, val); }
    void operator()(arg_type& a, arg_type& b) const
    {
        arg_type t = a;
        a = v_min(a, b);
        b = v_max(b, t);
    }
#if CV_SIMD_WIDTH > 16
    typedef v_uint8 warg_type;
    enum { WSIZE = v_uint8::nlanes };
    warg_type wload(const uchar* ptr) { return vx_load(ptr); }
    void store(uchar* ptr, const warg_type &val) { v_store(ptr, val); }
    void operator()(warg_type& a, warg_type& b) const
    {
        warg_type t = a;
        a = v_min(a, b);
        b = v_max(b, t);
    }
#endif
};


struct MinMaxVec16u
{
    typedef ushort value_type;
    typedef v_uint16x8 arg_type;
    enum { SIZE = v_uint16x8::nlanes };
    arg_type load(const ushort* ptr) { return v_load(ptr); }
    void store(ushort* ptr, const arg_type &val) { v_store(ptr, val); }
    void operator()(arg_type& a, arg_type& b) const
    {
        arg_type t = a;
        a = v_min(a, b);
        b = v_max(b, t);
    }
#if CV_SIMD_WIDTH > 16
    typedef v_uint16 warg_type;
    enum { WSIZE = v_uint16::nlanes };
    warg_type wload(const ushort* ptr) { return vx_load(ptr); }
    void store(ushort* ptr, const warg_type &val) { v_store(ptr, val); }
    void operator()(warg_type& a, warg_type& b) const
    {
        warg_type t = a;
        a = v_min(a, b);
        b = v_max(b, t);
    }
#endif
};


struct MinMaxVec16s
{
    typedef short value_type;
    typedef v_int16x8 arg_type;
    enum { SIZE = v_int16x8::nlanes };
    arg_type load(const short* ptr) { return v_load(ptr); }
    void store(short* ptr, const arg_type &val) { v_store(ptr, val); }
    void operator()(arg_type& a, arg_type& b) const
    {
        arg_type t = a;
        a = v_min(a, b);
        b = v_max(b, t);
    }
#if CV_SIMD_WIDTH > 16
    typedef v_int16 warg_type;
    enum { WSIZE = v_int16::nlanes };
    warg_type wload(const short* ptr) { return vx_load(ptr); }
    void store(short* ptr, const warg_type &val) { v_store(ptr, val); }
    void operator()(warg_type& a, warg_type& b) const
    {
        warg_type t = a;
        a = v_min(a, b);
        b = v_max(b, t);
    }
#endif
};


struct MinMaxVec32f
{
    typedef float value_type;
    typedef v_float32x4 arg_type;
    enum { SIZE = v_float32x4::nlanes };
    arg_type load(const float* ptr) { return v_load(ptr); }
    void store(float* ptr, const arg_type &val) { v_store(ptr, val); }
    void operator()(arg_type& a, arg_type& b) const
    {
        arg_type t = a;
        a = v_min(a, b);
        b = v_max(b, t);
    }
#if CV_SIMD_WIDTH > 16
    typedef v_float32 warg_type;
    enum { WSIZE = v_float32::nlanes };
    warg_type wload(const float* ptr) { return vx_load(ptr); }
    void store(float* ptr, const warg_type &val) { v_store(ptr, val); }
    void operator()(warg_type& a, warg_type& b) const
    {
        warg_type t = a;
        a = v_min(a, b);
        b = v_max(b, t);
    }
#endif
};

#else

typedef MinMax8u MinMaxVec8u;
typedef MinMax16u MinMaxVec16u;
typedef MinMax16s MinMaxVec16s;
typedef MinMax32f MinMaxVec32f;

#endif

template<class Op, class VecOp>
static void
medianBlur_SortNet( const Mat& _src, Mat& _dst, int m )
{
    typedef typename Op::value_type T;
    typedef typename Op::arg_type WT;
    typedef typename VecOp::arg_type VT;
#if CV_SIMD_WIDTH > 16
    typedef typename VecOp::warg_type WVT;
#endif

    const T* src = _src.ptr<T>();
    T* dst = _dst.ptr<T>();
    int sstep = (int)(_src.step/sizeof(T));
    int dstep = (int)(_dst.step/sizeof(T));
    Size size = _dst.size();
    int i, j, k, cn = _src.channels();
    Op op;
    VecOp vop;

    if( m == 3 )
    {
        if( size.width == 1 || size.height == 1 )
        {
            int len = size.width + size.height - 1;
            int sdelta = size.height == 1 ? cn : sstep;
            int sdelta0 = size.height == 1 ? 0 : sstep - cn;
            int ddelta = size.height == 1 ? cn : dstep;

            for( i = 0; i < len; i++, src += sdelta0, dst += ddelta )
                for( j = 0; j < cn; j++, src++ )
                {
                    WT p0 = src[i > 0 ? -sdelta : 0];
                    WT p1 = src[0];
                    WT p2 = src[i < len - 1 ? sdelta : 0];

                    op(p0, p1); op(p1, p2); op(p0, p1);
                    dst[j] = (T)p1;
                }
            return;
        }

        size.width *= cn;
        for( i = 0; i < size.height; i++, dst += dstep )
        {
            const T* row0 = src + std::max(i - 1, 0)*sstep;
            const T* row1 = src + i*sstep;
            const T* row2 = src + std::min(i + 1, size.height-1)*sstep;
            int limit = cn;

            for(j = 0;; )
            {
                for( ; j < limit; j++ )
                {
                    int j0 = j >= cn ? j - cn : j;
                    int j2 = j < size.width - cn ? j + cn : j;
                    WT p0 = row0[j0], p1 = row0[j], p2 = row0[j2];
                    WT p3 = row1[j0], p4 = row1[j], p5 = row1[j2];
                    WT p6 = row2[j0], p7 = row2[j], p8 = row2[j2];

                    op(p1, p2); op(p4, p5); op(p7, p8); op(p0, p1);
                    op(p3, p4); op(p6, p7); op(p1, p2); op(p4, p5);
                    op(p7, p8); op(p0, p3); op(p5, p8); op(p4, p7);
                    op(p3, p6); op(p1, p4); op(p2, p5); op(p4, p7);
                    op(p4, p2); op(p6, p4); op(p4, p2);
                    dst[j] = (T)p4;
                }

                if( limit == size.width )
                    break;

#if CV_SIMD_WIDTH > 16
                for( ; j <= size.width - VecOp::WSIZE - cn; j += VecOp::WSIZE )
                {
                    WVT p0 = vop.wload(row0+j-cn), p1 = vop.wload(row0+j), p2 = vop.wload(row0+j+cn);
                    WVT p3 = vop.wload(row1+j-cn), p4 = vop.wload(row1+j), p5 = vop.wload(row1+j+cn);
                    WVT p6 = vop.wload(row2+j-cn), p7 = vop.wload(row2+j), p8 = vop.wload(row2+j+cn);

                    vop(p1, p2); vop(p4, p5); vop(p7, p8); vop(p0, p1);
                    vop(p3, p4); vop(p6, p7); vop(p1, p2); vop(p4, p5);
                    vop(p7, p8); vop(p0, p3); vop(p5, p8); vop(p4, p7);
                    vop(p3, p6); vop(p1, p4); vop(p2, p5); vop(p4, p7);
                    vop(p4, p2); vop(p6, p4); vop(p4, p2);
                    vop.store(dst+j, p4);
                }
#endif
                for( ; j <= size.width - VecOp::SIZE - cn; j += VecOp::SIZE )
                {
                    VT p0 = vop.load(row0+j-cn), p1 = vop.load(row0+j), p2 = vop.load(row0+j+cn);
                    VT p3 = vop.load(row1+j-cn), p4 = vop.load(row1+j), p5 = vop.load(row1+j+cn);
                    VT p6 = vop.load(row2+j-cn), p7 = vop.load(row2+j), p8 = vop.load(row2+j+cn);

                    vop(p1, p2); vop(p4, p5); vop(p7, p8); vop(p0, p1);
                    vop(p3, p4); vop(p6, p7); vop(p1, p2); vop(p4, p5);
                    vop(p7, p8); vop(p0, p3); vop(p5, p8); vop(p4, p7);
                    vop(p3, p6); vop(p1, p4); vop(p2, p5); vop(p4, p7);
                    vop(p4, p2); vop(p6, p4); vop(p4, p2);
                    vop.store(dst+j, p4);
                }

                limit = size.width;
            }
        }
#if CV_SIMD
        vx_cleanup();
#endif
    }
    else if( m == 5 )
    {
        if( size.width == 1 || size.height == 1 )
        {
            int len = size.width + size.height - 1;
            int sdelta = size.height == 1 ? cn : sstep;
            int sdelta0 = size.height == 1 ? 0 : sstep - cn;
            int ddelta = size.height == 1 ? cn : dstep;

            for( i = 0; i < len; i++, src += sdelta0, dst += ddelta )
                for( j = 0; j < cn; j++, src++ )
                {
                    int i1 = i > 0 ? -sdelta : 0;
                    int i0 = i > 1 ? -sdelta*2 : i1;
                    int i3 = i < len-1 ? sdelta : 0;
                    int i4 = i < len-2 ? sdelta*2 : i3;
                    WT p0 = src[i0], p1 = src[i1], p2 = src[0], p3 = src[i3], p4 = src[i4];

                    op(p0, p1); op(p3, p4); op(p2, p3); op(p3, p4); op(p0, p2);
                    op(p2, p4); op(p1, p3); op(p1, p2);
                    dst[j] = (T)p2;
                }
            return;
        }

        size.width *= cn;
        for( i = 0; i < size.height; i++, dst += dstep )
        {
            const T* row[5];
            row[0] = src + std::max(i - 2, 0)*sstep;
            row[1] = src + std::max(i - 1, 0)*sstep;
            row[2] = src + i*sstep;
            row[3] = src + std::min(i + 1, size.height-1)*sstep;
            row[4] = src + std::min(i + 2, size.height-1)*sstep;
            int limit = cn*2;

            for(j = 0;; )
            {
                for( ; j < limit; j++ )
                {
                    WT p[25];
                    int j1 = j >= cn ? j - cn : j;
                    int j0 = j >= cn*2 ? j - cn*2 : j1;
                    int j3 = j < size.width - cn ? j + cn : j;
                    int j4 = j < size.width - cn*2 ? j + cn*2 : j3;
                    for( k = 0; k < 5; k++ )
                    {
                        const T* rowk = row[k];
                        p[k*5] = rowk[j0]; p[k*5+1] = rowk[j1];
                        p[k*5+2] = rowk[j]; p[k*5+3] = rowk[j3];
                        p[k*5+4] = rowk[j4];
                    }

                    op(p[1], p[2]); op(p[0], p[1]); op(p[1], p[2]); op(p[4], p[5]); op(p[3], p[4]);
                    op(p[4], p[5]); op(p[0], p[3]); op(p[2], p[5]); op(p[2], p[3]); op(p[1], p[4]);
                    op(p[1], p[2]); op(p[3], p[4]); op(p[7], p[8]); op(p[6], p[7]); op(p[7], p[8]);
                    op(p[10], p[11]); op(p[9], p[10]); op(p[10], p[11]); op(p[6], p[9]); op(p[8], p[11]);
                    op(p[8], p[9]); op(p[7], p[10]); op(p[7], p[8]); op(p[9], p[10]); op(p[0], p[6]);
                    op(p[4], p[10]); op(p[4], p[6]); op(p[2], p[8]); op(p[2], p[4]); op(p[6], p[8]);
                    op(p[1], p[7]); op(p[5], p[11]); op(p[5], p[7]); op(p[3], p[9]); op(p[3], p[5]);
                    op(p[7], p[9]); op(p[1], p[2]); op(p[3], p[4]); op(p[5], p[6]); op(p[7], p[8]);
                    op(p[9], p[10]); op(p[13], p[14]); op(p[12], p[13]); op(p[13], p[14]); op(p[16], p[17]);
                    op(p[15], p[16]); op(p[16], p[17]); op(p[12], p[15]); op(p[14], p[17]); op(p[14], p[15]);
                    op(p[13], p[16]); op(p[13], p[14]); op(p[15], p[16]); op(p[19], p[20]); op(p[18], p[19]);
                    op(p[19], p[20]); op(p[21], p[22]); op(p[23], p[24]); op(p[21], p[23]); op(p[22], p[24]);
                    op(p[22], p[23]); op(p[18], p[21]); op(p[20], p[23]); op(p[20], p[21]); op(p[19], p[22]);
                    op(p[22], p[24]); op(p[19], p[20]); op(p[21], p[22]); op(p[23], p[24]); op(p[12], p[18]);
                    op(p[16], p[22]); op(p[16], p[18]); op(p[14], p[20]); op(p[20], p[24]); op(p[14], p[16]);
                    op(p[18], p[20]); op(p[22], p[24]); op(p[13], p[19]); op(p[17], p[23]); op(p[17], p[19]);
                    op(p[15], p[21]); op(p[15], p[17]); op(p[19], p[21]); op(p[13], p[14]); op(p[15], p[16]);
                    op(p[17], p[18]); op(p[19], p[20]); op(p[21], p[22]); op(p[23], p[24]); op(p[0], p[12]);
                    op(p[8], p[20]); op(p[8], p[12]); op(p[4], p[16]); op(p[16], p[24]); op(p[12], p[16]);
                    op(p[2], p[14]); op(p[10], p[22]); op(p[10], p[14]); op(p[6], p[18]); op(p[6], p[10]);
                    op(p[10], p[12]); op(p[1], p[13]); op(p[9], p[21]); op(p[9], p[13]); op(p[5], p[17]);
                    op(p[13], p[17]); op(p[3], p[15]); op(p[11], p[23]); op(p[11], p[15]); op(p[7], p[19]);
                    op(p[7], p[11]); op(p[11], p[13]); op(p[11], p[12]);
                    dst[j] = (T)p[12];
                }

                if( limit == size.width )
                    break;

#if CV_SIMD_WIDTH > 16
                for( ; j <= size.width - VecOp::WSIZE - cn*2; j += VecOp::WSIZE )
                {
                    WVT p[25];
                    for( k = 0; k < 5; k++ )
                    {
                        const T* rowk = row[k];
                        p[k*5] = vop.wload(rowk+j-cn*2); p[k*5+1] = vop.wload(rowk+j-cn);
                        p[k*5+2] = vop.wload(rowk+j); p[k*5+3] = vop.wload(rowk+j+cn);
                        p[k*5+4] = vop.wload(rowk+j+cn*2);
                    }

                    vop(p[1], p[2]); vop(p[0], p[1]); vop(p[1], p[2]); vop(p[4], p[5]); vop(p[3], p[4]);
                    vop(p[4], p[5]); vop(p[0], p[3]); vop(p[2], p[5]); vop(p[2], p[3]); vop(p[1], p[4]);
                    vop(p[1], p[2]); vop(p[3], p[4]); vop(p[7], p[8]); vop(p[6], p[7]); vop(p[7], p[8]);
                    vop(p[10], p[11]); vop(p[9], p[10]); vop(p[10], p[11]); vop(p[6], p[9]); vop(p[8], p[11]);
                    vop(p[8], p[9]); vop(p[7], p[10]); vop(p[7], p[8]); vop(p[9], p[10]); vop(p[0], p[6]);
                    vop(p[4], p[10]); vop(p[4], p[6]); vop(p[2], p[8]); vop(p[2], p[4]); vop(p[6], p[8]);
                    vop(p[1], p[7]); vop(p[5], p[11]); vop(p[5], p[7]); vop(p[3], p[9]); vop(p[3], p[5]);
                    vop(p[7], p[9]); vop(p[1], p[2]); vop(p[3], p[4]); vop(p[5], p[6]); vop(p[7], p[8]);
                    vop(p[9], p[10]); vop(p[13], p[14]); vop(p[12], p[13]); vop(p[13], p[14]); vop(p[16], p[17]);
                    vop(p[15], p[16]); vop(p[16], p[17]); vop(p[12], p[15]); vop(p[14], p[17]); vop(p[14], p[15]);
                    vop(p[13], p[16]); vop(p[13], p[14]); vop(p[15], p[16]); vop(p[19], p[20]); vop(p[18], p[19]);
                    vop(p[19], p[20]); vop(p[21], p[22]); vop(p[23], p[24]); vop(p[21], p[23]); vop(p[22], p[24]);
                    vop(p[22], p[23]); vop(p[18], p[21]); vop(p[20], p[23]); vop(p[20], p[21]); vop(p[19], p[22]);
                    vop(p[22], p[24]); vop(p[19], p[20]); vop(p[21], p[22]); vop(p[23], p[24]); vop(p[12], p[18]);
                    vop(p[16], p[22]); vop(p[16], p[18]); vop(p[14], p[20]); vop(p[20], p[24]); vop(p[14], p[16]);
                    vop(p[18], p[20]); vop(p[22], p[24]); vop(p[13], p[19]); vop(p[17], p[23]); vop(p[17], p[19]);
                    vop(p[15], p[21]); vop(p[15], p[17]); vop(p[19], p[21]); vop(p[13], p[14]); vop(p[15], p[16]);
                    vop(p[17], p[18]); vop(p[19], p[20]); vop(p[21], p[22]); vop(p[23], p[24]); vop(p[0], p[12]);
                    vop(p[8], p[20]); vop(p[8], p[12]); vop(p[4], p[16]); vop(p[16], p[24]); vop(p[12], p[16]);
                    vop(p[2], p[14]); vop(p[10], p[22]); vop(p[10], p[14]); vop(p[6], p[18]); vop(p[6], p[10]);
                    vop(p[10], p[12]); vop(p[1], p[13]); vop(p[9], p[21]); vop(p[9], p[13]); vop(p[5], p[17]);
                    vop(p[13], p[17]); vop(p[3], p[15]); vop(p[11], p[23]); vop(p[11], p[15]); vop(p[7], p[19]);
                    vop(p[7], p[11]); vop(p[11], p[13]); vop(p[11], p[12]);
                    vop.store(dst+j, p[12]);
                }
#endif
                for( ; j <= size.width - VecOp::SIZE - cn*2; j += VecOp::SIZE )
                {
                    VT p[25];
                    for( k = 0; k < 5; k++ )
                    {
                        const T* rowk = row[k];
                        p[k*5] = vop.load(rowk+j-cn*2); p[k*5+1] = vop.load(rowk+j-cn);
                        p[k*5+2] = vop.load(rowk+j); p[k*5+3] = vop.load(rowk+j+cn);
                        p[k*5+4] = vop.load(rowk+j+cn*2);
                    }

                    vop(p[1], p[2]); vop(p[0], p[1]); vop(p[1], p[2]); vop(p[4], p[5]); vop(p[3], p[4]);
                    vop(p[4], p[5]); vop(p[0], p[3]); vop(p[2], p[5]); vop(p[2], p[3]); vop(p[1], p[4]);
                    vop(p[1], p[2]); vop(p[3], p[4]); vop(p[7], p[8]); vop(p[6], p[7]); vop(p[7], p[8]);
                    vop(p[10], p[11]); vop(p[9], p[10]); vop(p[10], p[11]); vop(p[6], p[9]); vop(p[8], p[11]);
                    vop(p[8], p[9]); vop(p[7], p[10]); vop(p[7], p[8]); vop(p[9], p[10]); vop(p[0], p[6]);
                    vop(p[4], p[10]); vop(p[4], p[6]); vop(p[2], p[8]); vop(p[2], p[4]); vop(p[6], p[8]);
                    vop(p[1], p[7]); vop(p[5], p[11]); vop(p[5], p[7]); vop(p[3], p[9]); vop(p[3], p[5]);
                    vop(p[7], p[9]); vop(p[1], p[2]); vop(p[3], p[4]); vop(p[5], p[6]); vop(p[7], p[8]);
                    vop(p[9], p[10]); vop(p[13], p[14]); vop(p[12], p[13]); vop(p[13], p[14]); vop(p[16], p[17]);
                    vop(p[15], p[16]); vop(p[16], p[17]); vop(p[12], p[15]); vop(p[14], p[17]); vop(p[14], p[15]);
                    vop(p[13], p[16]); vop(p[13], p[14]); vop(p[15], p[16]); vop(p[19], p[20]); vop(p[18], p[19]);
                    vop(p[19], p[20]); vop(p[21], p[22]); vop(p[23], p[24]); vop(p[21], p[23]); vop(p[22], p[24]);
                    vop(p[22], p[23]); vop(p[18], p[21]); vop(p[20], p[23]); vop(p[20], p[21]); vop(p[19], p[22]);
                    vop(p[22], p[24]); vop(p[19], p[20]); vop(p[21], p[22]); vop(p[23], p[24]); vop(p[12], p[18]);
                    vop(p[16], p[22]); vop(p[16], p[18]); vop(p[14], p[20]); vop(p[20], p[24]); vop(p[14], p[16]);
                    vop(p[18], p[20]); vop(p[22], p[24]); vop(p[13], p[19]); vop(p[17], p[23]); vop(p[17], p[19]);
                    vop(p[15], p[21]); vop(p[15], p[17]); vop(p[19], p[21]); vop(p[13], p[14]); vop(p[15], p[16]);
                    vop(p[17], p[18]); vop(p[19], p[20]); vop(p[21], p[22]); vop(p[23], p[24]); vop(p[0], p[12]);
                    vop(p[8], p[20]); vop(p[8], p[12]); vop(p[4], p[16]); vop(p[16], p[24]); vop(p[12], p[16]);
                    vop(p[2], p[14]); vop(p[10], p[22]); vop(p[10], p[14]); vop(p[6], p[18]); vop(p[6], p[10]);
                    vop(p[10], p[12]); vop(p[1], p[13]); vop(p[9], p[21]); vop(p[9], p[13]); vop(p[5], p[17]);
                    vop(p[13], p[17]); vop(p[3], p[15]); vop(p[11], p[23]); vop(p[11], p[15]); vop(p[7], p[19]);
                    vop(p[7], p[11]); vop(p[11], p[13]); vop(p[11], p[12]);
                    vop.store(dst+j, p[12]);
                }

                limit = size.width;
            }
        }
#if CV_SIMD
        vx_cleanup();
#endif
    }
}

#ifdef HAVE_OPENCL

#define DIVUP(total, grain) ((total + grain - 1) / (grain))

static bool ocl_medianFilter(InputArray _src, OutputArray _dst, int m)
{
    size_t localsize[2] = { 16, 16 };
    size_t globalsize[2];
    int type = _src.type(), depth = CV_MAT_DEPTH(type), cn = CV_MAT_CN(type);

    if ( !((depth == CV_8U || depth == CV_16U || depth == CV_16S || depth == CV_32F) && cn <= 4 && (m == 3 || m == 5)) )
        return false;

    Size imgSize = _src.size();
    bool useOptimized = (1 == cn) &&
                        (size_t)imgSize.width >= localsize[0] * 8  &&
                        (size_t)imgSize.height >= localsize[1] * 8 &&
                        imgSize.width % 4 == 0 &&
                        imgSize.height % 4 == 0 &&
                        (ocl::Device::getDefault().isIntel());

    cv::String kname = format( useOptimized ? "medianFilter%d_u" : "medianFilter%d", m) ;
    cv::String kdefs = useOptimized ?
                         format("-D T=%s -D T1=%s -D T4=%s%d -D cn=%d -D USE_4OPT", ocl::typeToStr(type),
                         ocl::typeToStr(depth), ocl::typeToStr(depth), cn*4, cn)
                         :
                         format("-D T=%s -D T1=%s -D cn=%d", ocl::typeToStr(type), ocl::typeToStr(depth), cn) ;

    ocl::Kernel k(kname.c_str(), ocl::imgproc::medianFilter_oclsrc, kdefs.c_str() );

    if (k.empty())
        return false;

    UMat src = _src.getUMat();
    _dst.create(src.size(), type);
    UMat dst = _dst.getUMat();

    k.args(ocl::KernelArg::ReadOnlyNoSize(src), ocl::KernelArg::WriteOnly(dst));

    if( useOptimized )
    {
        globalsize[0] = DIVUP(src.cols / 4, localsize[0]) * localsize[0];
        globalsize[1] = DIVUP(src.rows / 4, localsize[1]) * localsize[1];
    }
    else
    {
        globalsize[0] = (src.cols + localsize[0] + 2) / localsize[0] * localsize[0];
        globalsize[1] = (src.rows + localsize[1] - 1) / localsize[1] * localsize[1];
    }

    return k.run(2, globalsize, localsize, false);
}

#undef DIVUP

#endif

#ifdef HAVE_OPENVX
namespace ovx {
    template <> inline bool skipSmallImages<VX_KERNEL_MEDIAN_3x3>(int w, int h) { return w*h < 1280 * 720; }
}
static bool openvx_medianFilter(InputArray _src, OutputArray _dst, int ksize)
{
    if (_src.type() != CV_8UC1 || _dst.type() != CV_8U
#ifndef VX_VERSION_1_1
        || ksize != 3
#endif
        )
        return false;

    Mat src = _src.getMat();
    Mat dst = _dst.getMat();

    if (
#ifdef VX_VERSION_1_1
         ksize != 3 ? ovx::skipSmallImages<VX_KERNEL_NON_LINEAR_FILTER>(src.cols, src.rows) :
#endif
         ovx::skipSmallImages<VX_KERNEL_MEDIAN_3x3>(src.cols, src.rows)
       )
        return false;

    try
    {
        ivx::Context ctx = ovx::getOpenVXContext();
#ifdef VX_VERSION_1_1
        if ((vx_size)ksize > ctx.nonlinearMaxDimension())
            return false;
#endif

        Mat a;
        if (dst.data != src.data)
            a = src;
        else
            src.copyTo(a);

        ivx::Image
            ia = ivx::Image::createFromHandle(ctx, VX_DF_IMAGE_U8,
                ivx::Image::createAddressing(a.cols, a.rows, 1, (vx_int32)(a.step)), a.data),
            ib = ivx::Image::createFromHandle(ctx, VX_DF_IMAGE_U8,
                ivx::Image::createAddressing(dst.cols, dst.rows, 1, (vx_int32)(dst.step)), dst.data);

        //ATTENTION: VX_CONTEXT_IMMEDIATE_BORDER attribute change could lead to strange issues in multi-threaded environments
        //since OpenVX standard says nothing about thread-safety for now
        ivx::border_t prevBorder = ctx.immediateBorder();
        ctx.setImmediateBorder(VX_BORDER_REPLICATE);
#ifdef VX_VERSION_1_1
        if (ksize == 3)
#endif
        {
            ivx::IVX_CHECK_STATUS(vxuMedian3x3(ctx, ia, ib));
        }
#ifdef VX_VERSION_1_1
        else
        {
            ivx::Matrix mtx;
            if(ksize == 5)
                mtx = ivx::Matrix::createFromPattern(ctx, VX_PATTERN_BOX, ksize, ksize);
            else
            {
                vx_size supportedSize;
                ivx::IVX_CHECK_STATUS(vxQueryContext(ctx, VX_CONTEXT_NONLINEAR_MAX_DIMENSION, &supportedSize, sizeof(supportedSize)));
                if ((vx_size)ksize > supportedSize)
                {
                    ctx.setImmediateBorder(prevBorder);
                    return false;
                }
                Mat mask(ksize, ksize, CV_8UC1, Scalar(255));
                mtx = ivx::Matrix::create(ctx, VX_TYPE_UINT8, ksize, ksize);
                mtx.copyFrom(mask);
            }
            ivx::IVX_CHECK_STATUS(vxuNonLinearFilter(ctx, VX_NONLINEAR_FILTER_MEDIAN, ia, mtx, ib));
        }
#endif
        ctx.setImmediateBorder(prevBorder);
    }
    catch (const ivx::RuntimeError & e)
    {
        VX_DbgThrow(e.what());
    }
    catch (const ivx::WrapperError & e)
    {
        VX_DbgThrow(e.what());
    }

    return true;
}
#endif

#ifdef HAVE_IPP
static bool ipp_medianFilter(Mat &src0, Mat &dst, int ksize)
{
    CV_INSTRUMENT_REGION_IPP();

#if IPP_VERSION_X100 < 201801
    // Degradations for big kernel
    if(ksize > 7)
        return false;
#endif

    {
        int         bufSize;
        IppiSize    dstRoiSize = ippiSize(dst.cols, dst.rows), maskSize = ippiSize(ksize, ksize);
        IppDataType ippType = ippiGetDataType(src0.type());
        int         channels = src0.channels();
        IppAutoBuffer<Ipp8u> buffer;

        if(src0.isSubmatrix())
            return false;

        Mat src;
        if(dst.data != src0.data)
            src = src0;
        else
            src0.copyTo(src);

        if(ippiFilterMedianBorderGetBufferSize(dstRoiSize, maskSize, ippType, channels, &bufSize) < 0)
            return false;

        buffer.allocate(bufSize);

        switch(ippType)
        {
        case ipp8u:
            if(channels == 1)
                return CV_INSTRUMENT_FUN_IPP(ippiFilterMedianBorder_8u_C1R, src.ptr<Ipp8u>(), (int)src.step, dst.ptr<Ipp8u>(), (int)dst.step, dstRoiSize, maskSize, ippBorderRepl, 0, buffer) >= 0;
            else if(channels == 3)
                return CV_INSTRUMENT_FUN_IPP(ippiFilterMedianBorder_8u_C3R, src.ptr<Ipp8u>(), (int)src.step, dst.ptr<Ipp8u>(), (int)dst.step, dstRoiSize, maskSize, ippBorderRepl, 0, buffer) >= 0;
            else if(channels == 4)
                return CV_INSTRUMENT_FUN_IPP(ippiFilterMedianBorder_8u_C4R, src.ptr<Ipp8u>(), (int)src.step, dst.ptr<Ipp8u>(), (int)dst.step, dstRoiSize, maskSize, ippBorderRepl, 0, buffer) >= 0;
            else
                return false;
        case ipp16u:
            if(channels == 1)
                return CV_INSTRUMENT_FUN_IPP(ippiFilterMedianBorder_16u_C1R, src.ptr<Ipp16u>(), (int)src.step, dst.ptr<Ipp16u>(), (int)dst.step, dstRoiSize, maskSize, ippBorderRepl, 0, buffer) >= 0;
            else if(channels == 3)
                return CV_INSTRUMENT_FUN_IPP(ippiFilterMedianBorder_16u_C3R, src.ptr<Ipp16u>(), (int)src.step, dst.ptr<Ipp16u>(), (int)dst.step, dstRoiSize, maskSize, ippBorderRepl, 0, buffer) >= 0;
            else if(channels == 4)
                return CV_INSTRUMENT_FUN_IPP(ippiFilterMedianBorder_16u_C4R, src.ptr<Ipp16u>(), (int)src.step, dst.ptr<Ipp16u>(), (int)dst.step, dstRoiSize, maskSize, ippBorderRepl, 0, buffer) >= 0;
            else
                return false;
        case ipp16s:
            if(channels == 1)
                return CV_INSTRUMENT_FUN_IPP(ippiFilterMedianBorder_16s_C1R, src.ptr<Ipp16s>(), (int)src.step, dst.ptr<Ipp16s>(), (int)dst.step, dstRoiSize, maskSize, ippBorderRepl, 0, buffer) >= 0;
            else if(channels == 3)
                return CV_INSTRUMENT_FUN_IPP(ippiFilterMedianBorder_16s_C3R, src.ptr<Ipp16s>(), (int)src.step, dst.ptr<Ipp16s>(), (int)dst.step, dstRoiSize, maskSize, ippBorderRepl, 0, buffer) >= 0;
            else if(channels == 4)
                return CV_INSTRUMENT_FUN_IPP(ippiFilterMedianBorder_16s_C4R, src.ptr<Ipp16s>(), (int)src.step, dst.ptr<Ipp16s>(), (int)dst.step, dstRoiSize, maskSize, ippBorderRepl, 0, buffer) >= 0;
            else
                return false;
        case ipp32f:
            if(channels == 1)
                return CV_INSTRUMENT_FUN_IPP(ippiFilterMedianBorder_32f_C1R, src.ptr<Ipp32f>(), (int)src.step, dst.ptr<Ipp32f>(), (int)dst.step, dstRoiSize, maskSize, ippBorderRepl, 0, buffer) >= 0;
            else
                return false;
        default:
            return false;
        }
    }
}
#endif
}

void medianBlur( InputArray _src0, OutputArray _dst, int ksize )
{
    CV_INSTRUMENT_REGION();

    CV_Assert( (ksize % 2 == 1) && (_src0.dims() <= 2 ));

    if( ksize <= 1 || _src0.empty() )
    {
        _src0.copyTo(_dst);
        return;
    }

    CV_OCL_RUN(_dst.isUMat(),
               ocl_medianFilter(_src0,_dst, ksize))

    Mat src0 = _src0.getMat();
    _dst.create( src0.size(), src0.type() );
    Mat dst = _dst.getMat();

    CALL_HAL(medianBlur, cv_hal_medianBlur, src0.data, src0.step, dst.data, dst.step, src0.cols, src0.rows, src0.depth(),
             src0.channels(), ksize);

    CV_OVX_RUN(true,
               openvx_medianFilter(_src0, _dst, ksize))

    CV_IPP_RUN_FAST(ipp_medianFilter(src0, dst, ksize));

#ifdef HAVE_TEGRA_OPTIMIZATION
    if (tegra::useTegra() && tegra::medianBlur(src0, dst, ksize))
        return;
#endif

    bool useSortNet = ksize == 3 || (ksize == 5
#if !(CV_SIMD)
            && ( src0.depth() > CV_8U || src0.channels() == 2 || src0.channels() > 4 )
#endif
        );

    Mat src;
    if( useSortNet )
    {
        if( dst.data != src0.data )
            src = src0;
        else
            src0.copyTo(src);

        if( src.depth() == CV_8U )
            medianBlur_SortNet<MinMax8u, MinMaxVec8u>( src, dst, ksize );
        else if( src.depth() == CV_16U )
            medianBlur_SortNet<MinMax16u, MinMaxVec16u>( src, dst, ksize );
        else if( src.depth() == CV_16S )
            medianBlur_SortNet<MinMax16s, MinMaxVec16s>( src, dst, ksize );
        else if( src.depth() == CV_32F )
            medianBlur_SortNet<MinMax32f, MinMaxVec32f>( src, dst, ksize );
        else
            CV_Error(CV_StsUnsupportedFormat, "");

        return;
    }
    else
    {
        cv::copyMakeBorder( src0, src, 0, 0, ksize/2, ksize/2, BORDER_REPLICATE|BORDER_ISOLATED);

        int cn = src0.channels();
        CV_Assert( src.depth() == CV_8U && (cn == 1 || cn == 3 || cn == 4) );

        double img_size_mp = (double)(src0.total())/(1 << 20);
        if( ksize <= 3 + (img_size_mp < 1 ? 12 : img_size_mp < 4 ? 6 : 2)*
            (CV_SIMD ? 1 : 3))
            medianBlur_8u_Om( src, dst, ksize );
        else
            medianBlur_8u_O1( src, dst, ksize );
    }
}

}

/* End of file. */
