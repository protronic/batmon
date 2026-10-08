#ifndef BATMON_TEST_CASES_H
#define BATMON_TEST_CASES_H

#define BATMON_TEST_LIST(X)                                                                        \
	X(test_pylontech_original)                                                                 \
	X(test_emulation_and_vendors)                                                              \
	X(test_victron_extension)                                                                  \
	X(test_sma_dialect)                                                                        \
	X(test_pylon_hv)                                                                           \
	X(test_inverter_and_unknown)                                                               \
	X(test_id_table_overflow)

#define BATMON_TEST_DECL(name) void name(void);
BATMON_TEST_LIST(BATMON_TEST_DECL)
#undef BATMON_TEST_DECL

#endif
