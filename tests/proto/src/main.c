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

ZTEST(batmon_proto, test_ydt_frames)
{
	test_ydt_frames();
}

ZTEST(batmon_proto, test_serial_pylontech)
{
	test_serial_pylontech();
}

ZTEST(batmon_proto, test_serial_seplos)
{
	test_serial_seplos();
}

ZTEST(batmon_proto, test_serial_pace_state_machine)
{
	test_serial_pace_state_machine();
}

ZTEST(batmon_proto, test_serial_jk)
{
	test_serial_jk();
}

ZTEST(batmon_proto, test_serial_modbus)
{
	test_serial_modbus();
}

ZTEST(batmon_proto, test_serial_sniff)
{
	test_serial_sniff();
}
