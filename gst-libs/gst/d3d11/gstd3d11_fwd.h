/* GStreamer
 * Copyright (C) 2019 Seungha Yang <seungha.yang@navercorp.com>
 * Copyright (C) 2020 Seungha Yang <seungha@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __GST_D3D11_FWD_H__
#define __GST_D3D11_FWD_H__

#include <gst/gst.h>
#include <gst/d3d11/gstd3d11config.h>
#include <gst/d3d11/d3d11-prelude.h>

#ifndef INITGUID
#include <initguid.h>
#endif

#if (GST_D3D11_HEADER_VERSION >= 4)
#include <d3d11_4.h>
#elif (GST_D3D11_HEADER_VERSION >= 3)
#include <d3d11_3.h>
#elif (GST_D3D11_HEADER_VERSION >= 2)
#include <d3d11_2.h>
#elif (GST_D3D11_HEADER_VERSION >= 1)
#include <d3d11_1.h>
#else
#include <d3d11.h>
#endif

#if (GST_D3D11_DXGI_HEADER_VERSION >= 6)
#include <dxgi1_6.h>
#elif (GST_D3D11_DXGI_HEADER_VERSION >= 5)
#include <dxgi1_5.h>
#elif (GST_D3D11_DXGI_HEADER_VERSION >= 4)
#include <dxgi1_4.h>
#elif (GST_D3D11_DXGI_HEADER_VERSION >= 3)
#include <dxgi1_3.h>
#elif (GST_D3D11_DXGI_HEADER_VERSION >= 2)
#include <dxgi1_2.h>
#else
#include <dxgi.h>
#endif

G_BEGIN_DECLS

typedef struct _GstD3D11Device GstD3D11Device;
typedef struct _GstD3D11DeviceClass GstD3D11DeviceClass;
typedef struct _GstD3D11DevicePrivate GstD3D11DevicePrivate;

typedef struct _GstD3D11AllocationParams GstD3D11AllocationParams;
typedef struct _GstD3D11Memory GstD3D11Memory;
typedef struct _GstD3D11MemoryPrivate GstD3D11MemoryPrivate;

typedef struct _GstD3D11Allocator GstD3D11Allocator;
typedef struct _GstD3D11AllocatorClass GstD3D11AllocatorClass;
typedef struct _GstD3D11AllocatorPrivate GstD3D11AllocatorPrivate;

typedef struct _GstD3D11PoolAllocator GstD3D11PoolAllocator;
typedef struct _GstD3D11PoolAllocatorClass GstD3D11PoolAllocatorClass;
typedef struct _GstD3D11PoolAllocatorPrivate GstD3D11PoolAllocatorPrivate;

typedef struct _GstD3D11BufferPool GstD3D11BufferPool;
typedef struct _GstD3D11BufferPoolClass GstD3D11BufferPoolClass;
typedef struct _GstD3D11BufferPoolPrivate GstD3D11BufferPoolPrivate;

typedef struct _GstD3D11Format GstD3D11Format;

G_END_DECLS

#endif /* __GST_D3D11_FWD_H__ */
