// Detector puro do modo de sobrevivencia (issue 12). O relogio e a memoria
// retida pertencem a HAL; este teste entrega uma sequencia sintetica de
// carimbos e observa apenas a decisao publica.
#include <unity.h>

#include "core/boot_loop.h"

using namespace core;

void test_three_resets_inside_thirty_seconds_declare_boot_loop(void) {
    const uint64_t resets_us[] = {0, 10'000'000, 20'000'000};

    const BootLoopStatus status = detect_boot_loop(resets_us, 3);

    TEST_ASSERT_TRUE(status.active);
    TEST_ASSERT_EQUAL_UINT8(3, status.recent_reset_count);
}

void test_two_resets_do_not_declare_boot_loop(void) {
    const uint64_t resets_us[] = {0, 10'000'000};

    const BootLoopStatus status = detect_boot_loop(resets_us, 2);

    TEST_ASSERT_FALSE(status.active);
    TEST_ASSERT_EQUAL_UINT8(2, status.recent_reset_count);
}

void test_resets_spanning_exactly_thirty_seconds_are_outside_the_window(void) {
    const uint64_t resets_us[] = {0, 15'000'000, 30'000'000};

    const BootLoopStatus status = detect_boot_loop(resets_us, 3);

    TEST_ASSERT_FALSE(status.active);
    TEST_ASSERT_EQUAL_UINT8(2, status.recent_reset_count);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_three_resets_inside_thirty_seconds_declare_boot_loop);
    RUN_TEST(test_two_resets_do_not_declare_boot_loop);
    RUN_TEST(test_resets_spanning_exactly_thirty_seconds_are_outside_the_window);
    return UNITY_END();
}
