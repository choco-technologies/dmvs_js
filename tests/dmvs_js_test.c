#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmvs_js.h"

static dmvs_js_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmvs_js_create();
}

void dmod_test_teardown(void)
{
    dmvs_js_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmvs_js_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmvs_js_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmvs_js_is_valid(g_handle));
}

DMOD_TEST_STEP(dmvs_js_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmvs_js_destroy(NULL);
}
