#include "teder.h"
#include "teder_notcurses.h"
#include "teder_version.h"
#include <check.h>
#include <string.h>

START_TEST(valid)
{
	teder_build_info info = {0};
	info.struct_size = sizeof(info);
	info.abi_version = TEDER_ABI_VERSION;
	ck_assert_int_eq(teder_get_build_info(&info), TEDER_OK);
	ck_assert_uint_eq(info.struct_size, sizeof(info));
	ck_assert_uint_eq(info.abi_version, 1);
	ck_assert_uint_eq(info.version_major, TEDER_VERSION_MAJOR);
	ck_assert_uint_eq(info.version_minor, TEDER_VERSION_MINOR);
	ck_assert_uint_eq(info.version_patch, TEDER_VERSION_PATCH);
	ck_assert_uint_eq(info.capabilities, 0);
	ck_assert_uint_eq(info.backend_mask, 0);
	ck_assert_uint_eq(info.reserved, 0);
}
END_TEST

START_TEST(invalid)
{
	teder_build_info info, before;
	ck_assert_int_eq(teder_get_build_info(NULL), TEDER_INVALID);
	memset(&info, 0xa5, sizeof(info));
	info.struct_size = sizeof(info) - 1;
	info.abi_version = TEDER_ABI_VERSION;
	memcpy(&before, &info, sizeof(info));
	ck_assert_int_eq(teder_get_build_info(&info), TEDER_ABI_MISMATCH);
	ck_assert_int_eq(memcmp(&info, &before, sizeof(info)), 0);
	info.struct_size = sizeof(info);
	info.abi_version = TEDER_ABI_VERSION + 1;
	memcpy(&before, &info, sizeof(info));
	ck_assert_int_eq(teder_get_build_info(&info), TEDER_ABI_MISMATCH);
	ck_assert_int_eq(memcmp(&info, &before, sizeof(info)), 0);
}
END_TEST

int main(int argc, char **argv)
{
	Suite *suite;
	TCase *test;
	SRunner *runner;
	int failed;
	if (argc != 2 || (strcmp(argv[1], "valid") && strcmp(argv[1], "invalid")))
		return 2;
	suite = suite_create("build");
	test = tcase_create(argv[1]);
	if (!strcmp(argv[1], "valid"))
		tcase_add_test(test, valid);
	else
		tcase_add_test(test, invalid);
	suite_add_tcase(suite, test);
	runner = srunner_create(suite);
	srunner_set_fork_status(runner, CK_NOFORK);
	srunner_run_all(runner, CK_NORMAL);
	failed = srunner_ntests_failed(runner);
	srunner_free(runner);
	return failed ? 1 : 0;
}
