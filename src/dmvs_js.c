#define DMOD_ENABLE_REGISTRATION ON
#include "dmod.h"
#include "dmvs_js_types.h"

int dmod_init(const Dmod_Config_t* config)
{
    (void)config;
    return 0;
}

int dmod_deinit(void)
{
    return 0;
}
