#ifndef BATMON_HOST_CHECK_H
#define BATMON_HOST_CHECK_H

#include <stdio.h>

extern int bm_test_fails;

#define CHECK(cond)                                                                           \
	do {                                                                                  \
		if (!(cond)) {                                                                \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
			bm_test_fails++;                                                      \
		}                                                                             \
	} while (0)

#endif
