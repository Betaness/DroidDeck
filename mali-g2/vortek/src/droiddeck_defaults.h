/*
 * DroidDeck G2 defaults for the Vortek client, force-included by build-client.sh.
 * The session also exports VORTEK_SERVER_PATH; this default only matters for
 * programs that start with a scrubbed environment.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef VORTEK_SERVER_PATH
#define VORTEK_SERVER_PATH "/run/droiddeck/vortek/V0"
#endif

/* [mali-g2] DroidDeck-only client changes (vortek-0003 and later). */
#define VORTEK_DROIDDECK 1
