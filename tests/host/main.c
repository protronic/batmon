/* Host runner: make -C tests/host  (or see Makefile for the plain gcc line) */
#include <stdio.h>

#include "check.h"
#include "test_cases.h"

int bm_test_fails;

int main(void)
{
#define RUN(name)                                                                             \
	do {                                                                                  \
		int before = bm_test_fails;                                                   \
		name();                                                                       \
		printf("%-32s %s\n", #name, bm_test_fails == before ? "ok" : "FAILED");       \
	} while (0);
	BATMON_TEST_LIST(RUN)
#undef RUN
	if (bm_test_fails) {
		printf("%d FAILURES\n", bm_test_fails);
		return 1;
	}
	printf("all protocol tests passed\n");
	return 0;
}
