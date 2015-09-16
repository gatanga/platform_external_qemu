// Copyright 2015 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#define __STDC_LIMIT_MACROS
#include "android/metrics/metrics_reporter_ga.h"
#include "android/metrics/internal/metrics_reporter_ga_internal.h"

#include <gtest/gtest.h>

TEST(MetricsReporterGa, defaultMetrics) {
    char* post_data;
    AndroidMetrics metrics;
    static const char kExpected[] =
            "v=1&tid=UA-19996407-3&an=Android Studio&av=unknown&"
            "cid=default-client&cd6=unknown&t=event&ec=emulator&"
            "ea=singleRunCrashInfo&el=crashDetected&cm2=0&cm3=0";
    static const int kExpectedLen = (int)(sizeof(kExpected) - 1);

    androidMetrics_init(&metrics);
    ASSERT_EQ(kExpectedLen, formatGAPostData(&post_data, &metrics));
    ASSERT_STREQ(kExpected, post_data);
    androidMetrics_fini(&metrics);
    free(post_data);
}

TEST(MetricsReporterGa, cleanRun) {
    char* post_data;
    AndroidMetrics metrics;
    static const char kExpected[] =
            "v=1&tid=UA-19996407-3&an=Android Studio&av=standalone&"
            "cid=default-client&cd6=x86_64&t=event&ec=emulator&"
            "ea=singleRunCrashInfo&el=cleanExit&cm2=220&cm3=1170";
    static const int kExpectedLen = (int)(sizeof(kExpected) - 1);

    androidMetrics_init(&metrics);
    ANDROID_METRICS_STRASSIGN(metrics.emulator_version, "standalone");
    ANDROID_METRICS_STRASSIGN(metrics.guest_arch, "x86_64");
    metrics.guest_gpu_enabled = 1;
    metrics.tick = 1;
    metrics.system_time = 1170;
    metrics.user_time = 220;
    metrics.is_dirty = 0;
    metrics.num_failed_reports = 7;

    ASSERT_EQ(kExpectedLen, formatGAPostData(&post_data, &metrics));
    ASSERT_STREQ(kExpected, post_data);
    androidMetrics_fini(&metrics);
    free(post_data);
}

TEST(MetricsReporterGa, dirtyRun) {
    char* post_data;
    AndroidMetrics metrics;
    static const char kExpected[] =
            "v=1&tid=UA-19996407-3&an=Android Studio&av=standalone&"
            "cid=default-client&cd6=x86_64&t=event&ec=emulator&"
            "ea=singleRunCrashInfo&el=crashDetected&cm2=180&cm3=1080";
    static const int kExpectedLen = (int)(sizeof(kExpected) - 1);

    androidMetrics_init(&metrics);
    ANDROID_METRICS_STRASSIGN(metrics.emulator_version, "standalone");
    ANDROID_METRICS_STRASSIGN(metrics.guest_arch, "x86_64");
    metrics.guest_gpu_enabled = 1;
    metrics.tick = 1;
    metrics.system_time = 1080;
    metrics.user_time = 180;
    metrics.is_dirty = 1;
    metrics.num_failed_reports = 9;

    ASSERT_EQ(kExpectedLen, formatGAPostData(&post_data, &metrics));
    ASSERT_STREQ(kExpected, post_data);
    androidMetrics_fini(&metrics);
    free(post_data);
}
