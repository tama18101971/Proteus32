/* ---------------------------------------------------------------------------
 * size_probe.c — minimal program that pulls in the whole library
 *
 * Used by the size_* build environments to measure the library's Flash and
 * RAM cost without an example application on top. Every public entry point is
 * referenced so that -Wl,-gc-sections cannot drop any of them.
 *
 * Not part of the library: the size_* envs pick it up explicitly.
 * ------------------------------------------------------------------------- */
#include "wl_flash.h"

int main(void)
{
    if (wl_load() != WL_OK)
    {
        wl_init();
    }

    *(volatile uint8_t *)wl_data() = 1u;
    wl_mark_dirty();

    if (wl_is_dirty())
    {
        (void)wl_commit_if_dirty();
    }
    (void)wl_commit();

    while (1)
    {
    }
}
