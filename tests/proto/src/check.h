#ifndef BATMON_ZTEST_CHECK_H
#define BATMON_ZTEST_CHECK_H

#include <zephyr/ztest.h>

#define CHECK(cond) zassert_true((cond), "%s", #cond)

#endif
