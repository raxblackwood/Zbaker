// Copyright 2009-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#define RTC_VERSION_MAJOR 3
#define RTC_VERSION_MINOR 13
#define RTC_VERSION_PATCH 5
#define RTC_VERSION 31305
#define RTC_VERSION_STRING "3.13.5"

#define RTC_MAX_INSTANCE_LEVEL_COUNT 1
#define RTC_MIN_WIDTH 0

#define RTC_NAMESPACE_BEGIN
#define RTC_NAMESPACE_END
#define RTC_NAMESPACE_USE

#if defined(__cplusplus)
#  define RTC_API_EXTERN_C extern "C"
#else
#  define RTC_API_EXTERN_C
#endif

#if defined(_WIN32)
#  define RTC_API_IMPORT RTC_API_EXTERN_C __declspec(dllimport)
#  define RTC_API_EXPORT RTC_API_EXTERN_C __declspec(dllexport)
#else
#  define RTC_API_IMPORT RTC_API_EXTERN_C
#  define RTC_API_EXPORT RTC_API_EXTERN_C __attribute__ ((visibility ("default")))
#endif

#if defined(RTC_EXPORT_API)
#  define RTC_API RTC_API_EXPORT
#else
#  define RTC_API RTC_API_IMPORT
#endif
