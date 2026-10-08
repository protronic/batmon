#include <zephyr/ztest.h>

#include "test_cases.h"

ZTEST_SUITE(batmon_proto, NULL, NULL, NULL, NULL, NULL);

ZTEST(batmon_proto, test_pylontech_original)
{
	test_pylontech_original();
}

ZTEST(batmon_proto, test_emulation_and_vendors)
{
	test_emulation_and_vendors();
}

ZTEST(batmon_proto, test_victron_extension)
{
	test_victron_extension();
}

ZTEST(batmon_proto, test_sma_dialect)
{
	test_sma_dialect();
}

ZTEST(batmon_proto, test_pylon_hv)
{
	test_pylon_hv();
}

ZTEST(batmon_proto, test_inverter_and_unknown)
{
	test_inverter_and_unknown();
}

ZTEST(batmon_proto, test_id_table_overflow)
{
	test_id_table_overflow();
}
